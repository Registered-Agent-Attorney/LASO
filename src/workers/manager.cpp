#include <algorithm>
#include <cctype>
#include <cmath>
#include <laso/core/config.hpp>
#include <laso/workers/manager.hpp>
#include <laso/workers/process_protocol.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <thread>

namespace laso {
namespace {
constexpr auto max_job_id_bytes = std::size_t{128};
constexpr auto max_external_id_bytes = std::size_t{512};
constexpr auto max_metadata_bytes = std::size_t{64} * 1024;
constexpr auto max_result_bytes = std::size_t{1024} * 1024;
constexpr auto max_artifact_metadata_bytes = std::size_t{64} * 1024;
constexpr auto max_usage_metadata_bytes = std::size_t{64} * 1024;
constexpr auto max_usage_identity_bytes = std::size_t{128};
constexpr std::uint64_t max_usage_metric = 1000000000000000ULL;

bool bounded_identifier(const std::string &value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':';
         });
}

bool browser_tool_child_needs_result(const WorkerJob &job) {
  return job.state == WorkerJobState::Completed &&
         job.request_metadata.contains("parent_worker_job_id") && !job.external_job_id.empty() &&
         !job.request_metadata.value("codex_tool_result_retrieved", false);
}

bool valid_browser_status_result(const Json &result) {
  if (!result.is_object() || !result.contains("window_count") ||
      !result.contains("browser_status") || !result.at("browser_status").is_object() ||
      result.at("browser_status").empty())
    return false;
  const auto &count = result.at("window_count");
  return count.is_number_unsigned() ||
         (count.is_number_integer() && count.get<std::int64_t>() >= 0);
}

std::string bounded_error(const std::string &value) {
  if (value.size() <= 512)
    return value;
  return value.substr(0, 512);
}

void apply_continuation(WorkerJob &job,
                        const std::optional<OpaqueProviderContinuation> &continuation) {
  if (!continuation)
    return;
  if (continuation->provider_id.empty() || continuation->provider_id.size() > 256 ||
      continuation->provider_version.empty() || continuation->provider_version.size() > 128 ||
      continuation->state.empty() || continuation->state.size() > 64 * 1024)
    throw WorkerTransportError("Worker returned invalid continuation state");
  job.continuation = continuation;
}

WorkerJobState event_state(const std::string &type) {
  if (type == "worker.job.started")
    return WorkerJobState::Running;
  if (type == "worker.job.completed")
    return WorkerJobState::Completed;
  if (type == "worker.job.failed")
    return WorkerJobState::Failed;
  if (type == "worker.job.cancelled")
    return WorkerJobState::Cancelled;
  return WorkerJobState::Unknown;
}
} // namespace

WorkerManager::WorkerManager(Storage &storage, WorkerRegistry &registry, unsigned max_active,
                             unsigned max_per_worker, unsigned max_artifacts,
                             std::uint64_t max_wall_time_ms, std::uint64_t max_tokens_per_run,
                             double max_cost_units_per_run)
    : storage_(storage), registry_(registry), max_active_(max_active),
      max_per_worker_(max_per_worker), max_artifacts_(max_artifacts),
      max_wall_time_ms_(max_wall_time_ms), max_tokens_per_run_(max_tokens_per_run),
      max_cost_units_per_run_(max_cost_units_per_run) {
  if (max_active_ == 0 || max_active_ > 4096 || max_per_worker_ == 0 ||
      max_per_worker_ > max_active_ || max_artifacts_ == 0 || max_artifacts_ > 64 ||
      max_wall_time_ms_ > max_usage_metric || max_tokens_per_run_ > max_usage_metric ||
      !std::isfinite(max_cost_units_per_run_) || max_cost_units_per_run_ < 0 ||
      max_cost_units_per_run_ > static_cast<double>(max_usage_metric))
    throw Error(ErrorCode::Configuration, "Invalid worker limits");
}

WorkerManager::WorkerManager(Storage &storage, WorkerRegistry &registry, Policy &policy,
                             unsigned max_active, unsigned max_per_worker, unsigned max_artifacts,
                             std::uint64_t max_wall_time_ms, std::uint64_t max_tokens_per_run,
                             double max_cost_units_per_run)
    : WorkerManager(storage, registry, max_active, max_per_worker, max_artifacts, max_wall_time_ms,
                    max_tokens_per_run, max_cost_units_per_run) {
  policy_ = &policy;
}

WorkerJob WorkerManager::job(const std::string &id) const {
  if (!bounded_identifier(id, max_job_id_bytes))
    throw Error(ErrorCode::Validation, "Invalid worker job id");
  return storage_.get(RecordKind::WorkerJob, id).get<WorkerJob>();
}

std::optional<OpaqueProviderContinuation>
WorkerManager::continuation_candidate(const std::string &id) const {
  if (!bounded_identifier(id, max_job_id_bytes))
    throw Error(ErrorCode::Validation, "Invalid worker job id");
  return job(id).continuation;
}

std::string WorkerManager::job_id_for(const std::string &idempotency_key) const {
  if (idempotency_key.empty() || idempotency_key.size() > 512)
    throw Error(ErrorCode::Validation, "Invalid worker idempotency key");
  return durable_job_id(idempotency_key);
}

WorkerJob WorkerManager::refresh(const std::string &id) {
  WorkerJob value;
  {
    std::lock_guard state_lock(state_mutex_);
    value = job(id);
    fence_stale_submission_locked(value);
  }
  if ((worker_job_terminal(value.state) && !browser_tool_child_needs_result(value)) ||
      value.external_job_id.empty())
    return value;
  return reconcile(std::move(value), true);
}

std::vector<Json> WorkerManager::jobs(const std::string &run_id, std::size_t limit,
                                      std::size_t offset) const {
  auto result = storage_.list(RecordKind::WorkerJob, run_id, limit, offset);
  for (auto &value : result)
    value.erase("_continuation_candidate");
  return result;
}

Json WorkerManager::workers() const {
  Json result = Json::array();
  for (const auto &name : registry_.names())
    result.push_back(registry_.get(name)->metadata());
  return result;
}

Json WorkerManager::worker(const std::string &id) const {
  return registry_.get(id)->metadata();
}

bool WorkerManager::has_completed_browser_status_tool_result(const std::string &parent_job_id,
                                                             const std::string &run_id) const {
  try {
    const auto parent = job(parent_job_id);
    if (parent.run_id != run_id || parent.node_id != "agent_three" ||
        parent.worker_id != "codex_agent_three" || parent.state != WorkerJobState::Completed ||
        parent.request_metadata.value("required_tool", std::string{}) != "laso.browser_status" ||
        parent.result_metadata.value("provider", std::string{}) != "openai" ||
        parent.result_metadata.value("model", std::string{}) != "gpt-6-luna" ||
        parent.result_metadata.value("reasoningEffort", std::string{}) != "high")
      return false;
    const auto session_id = parent.result_metadata.value("codex_session_id", std::string{});
    const auto turn_id = parent.result_metadata.value("codex_turn_id", std::string{});
    if (session_id.empty() || turn_id.empty())
      return false;
    const auto calls = parent.request_metadata.value("codex_browser_status_calls", Json::array());
    if (!calls.is_array() || calls.size() != 1)
      return false;
    const auto &call = calls.front();
    if (call.value("tool", std::string{}) != "laso.browser_status" ||
        call.value("worker_id", std::string{}) != "windows_computer" ||
        call.value("run_id", std::string{}) != run_id ||
        call.value("session_id", std::string{}) != session_id ||
        call.value("turn_id", std::string{}) != turn_id || !call.value("result_retrieved", false))
      return false;
    const auto child_id = call.value("child_worker_job_id", std::string{});
    const auto call_key = call.value("call_key", std::string{});
    if (child_id.empty() || call_key.empty())
      return false;
    const auto child = job(child_id);
    return child.run_id == run_id && child.worker_id == "windows_computer" &&
           child.node_id == "agent_three.browser_status" &&
           child.state == WorkerJobState::Completed &&
           child.request_metadata.value("task_type", std::string{}) == "browser.status" &&
           child.request_metadata.value("capability", std::string{}) == "browser.status" &&
           child.request_metadata.value("parent_worker_job_id", std::string{}) == parent.id &&
           child.request_metadata.value("parent_tool_call_id", std::string{}) == call_key &&
           child.request_metadata.value("parent_codex_session_id", std::string{}) == session_id &&
           child.request_metadata.value("parent_codex_turn_id", std::string{}) == turn_id &&
           child.request_metadata.value("codex_tool_result_retrieved", false) &&
           valid_browser_status_result(child.result);
  } catch (...) {
    return false;
  }
}

bool WorkerManager::can_execute(const std::string &worker_id, const std::string &capability) const {
  if (worker_id.empty() && capability.empty())
    return true;
  for (const auto &candidate : registry_.names()) {
    if (!worker_id.empty() && candidate != worker_id)
      continue;
    const auto adapter = registry_.get(candidate);
    const auto metadata = adapter->metadata();
    const auto recoverable = metadata.status == "failed" &&
                             (metadata.supports_recovery || adapter->supports_transport_restart());
    if (!metadata.enabled || (!metadata.healthy && !recoverable) ||
        (metadata.status != "healthy" && !recoverable))
      continue;
    if (!capability.empty() && std::find(metadata.capabilities.begin(), metadata.capabilities.end(),
                                         capability) == metadata.capabilities.end())
      continue;
    return true;
  }
  return false;
}

Json WorkerManager::distributed_capabilities() const {
  Json result{{"protocol_version", 1}, {"workloads", Json::array()}};
  for (const auto &candidate : registry_.names()) {
    const auto adapter = registry_.get(candidate);
    const auto metadata = adapter->metadata();
    const auto recoverable = metadata.status == "failed" &&
                             (metadata.supports_recovery || adapter->supports_transport_restart());
    if (!metadata.enabled || (!metadata.healthy && !recoverable) ||
        (metadata.status != "healthy" && !recoverable))
      continue;
    result["workloads"].push_back({{"worker_id", metadata.id},
                                   {"capabilities", metadata.capabilities},
                                   {"supports_status", metadata.supports_status},
                                   {"supports_recovery", metadata.supports_recovery},
                                   {"supports_cancellation", metadata.supports_cancellation},
                                   {"workspace_transport", true}});
  }
  if (result.dump().size() > 4096)
    throw Error(ErrorCode::Configuration, "Worker capability advertisement is too large");
  return result;
}

std::string WorkerManager::resolve_worker(const std::string &worker_id,
                                          const std::string &capability) const {
  if (!worker_id.empty()) {
    (void)registry_.get(worker_id);
    return worker_id;
  }
  if (capability.empty())
    throw Error(ErrorCode::Validation, "Worker id or capability is required");
  std::string result;
  for (const auto &candidate : registry_.names()) {
    const auto metadata = registry_.get(candidate)->metadata();
    if (std::find(metadata.capabilities.begin(), metadata.capabilities.end(), capability) ==
        metadata.capabilities.end())
      continue;
    if (!result.empty())
      throw Error(ErrorCode::Conflict, "Worker capability is ambiguous");
    result = candidate;
  }
  if (result.empty())
    throw Error(ErrorCode::NotFound, "No worker provides the requested capability");
  return result;
}

WorkerJobState WorkerManager::parse_state(const Json &value, WorkerJobState fallback) {
  try {
    return value.get<WorkerJobState>();
  } catch (...) {
    return fallback;
  }
}

std::string WorkerManager::durable_job_id(const std::string &idempotency_key) {
  // The identifier is a bounded storage key, not a security primitive.  The
  // full idempotency key remains in the job record so a hash collision is
  // detected and rejected rather than silently reusing another job.
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto byte : idempotency_key) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << "worker-" << std::hex << hash;
  return out.str();
}

void WorkerManager::merge_usage(WorkerUsage &target, const WorkerUsage &incoming) {
  if (incoming.queue_duration_ms)
    target.queue_duration_ms = incoming.queue_duration_ms;
  if (incoming.wall_duration_ms)
    target.wall_duration_ms = incoming.wall_duration_ms;
  if (incoming.input_tokens)
    target.input_tokens = incoming.input_tokens;
  if (incoming.output_tokens)
    target.output_tokens = incoming.output_tokens;
  if (incoming.total_tokens)
    target.total_tokens = incoming.total_tokens;
  if (incoming.tool_calls)
    target.tool_calls = incoming.tool_calls;
  if (incoming.action_count)
    target.action_count = incoming.action_count;
  if (incoming.cost_units)
    target.cost_units = incoming.cost_units;
  if (!incoming.provider.empty())
    target.provider = incoming.provider;
  if (!incoming.model.empty())
    target.model = incoming.model;
  if (!incoming.executor.empty())
    target.executor = incoming.executor;
  if (incoming.metadata.is_object())
    target.metadata.update(incoming.metadata);
  if (!target.total_tokens && target.input_tokens && target.output_tokens &&
      *target.input_tokens <= std::numeric_limits<std::uint64_t>::max() - *target.output_tokens)
    target.total_tokens = *target.input_tokens + *target.output_tokens;
}

std::optional<std::uint64_t> WorkerManager::total_tokens(const WorkerUsage &usage) {
  if (usage.total_tokens)
    return usage.total_tokens;
  if (usage.input_tokens && usage.output_tokens &&
      *usage.input_tokens <= std::numeric_limits<std::uint64_t>::max() - *usage.output_tokens)
    return *usage.input_tokens + *usage.output_tokens;
  return std::nullopt;
}

std::string WorkerManager::budget_violation(const WorkerJob &value) const {
  const auto metric_exceeds = [](const auto &metric) {
    return metric && *metric > max_usage_metric;
  };
  if (metric_exceeds(value.usage.queue_duration_ms) ||
      metric_exceeds(value.usage.wall_duration_ms) || metric_exceeds(value.usage.input_tokens) ||
      metric_exceeds(value.usage.output_tokens) || metric_exceeds(value.usage.total_tokens) ||
      metric_exceeds(value.usage.tool_calls) || metric_exceeds(value.usage.action_count))
    return "reported worker usage exceeds the representable limit";
  if (value.usage.metadata.dump().size() > max_usage_metadata_bytes)
    return "worker usage metadata exceeds the limit";
  if (value.usage.provider.size() > max_usage_identity_bytes ||
      value.usage.model.size() > max_usage_identity_bytes ||
      value.usage.executor.size() > max_usage_identity_bytes)
    return "worker usage identity exceeds the limit";
  if (value.usage.cost_units &&
      (!std::isfinite(*value.usage.cost_units) || *value.usage.cost_units < 0 ||
       *value.usage.cost_units > static_cast<double>(max_usage_metric)))
    return "reported worker cost_units is invalid";
  if (max_wall_time_ms_ != 0 && value.usage.wall_duration_ms &&
      *value.usage.wall_duration_ms > max_wall_time_ms_)
    return "worker wall-time budget exceeded";

  std::uint64_t prior_tokens = 0;
  double prior_cost = 0;
  if (max_tokens_per_run_ != 0 || max_cost_units_per_run_ != 0) {
    for (const auto &record : storage_.list(RecordKind::WorkerJob, value.run_id, 10000, 0)) {
      const auto prior = record.get<WorkerJob>();
      if (prior.id == value.id)
        continue;
      if (const auto tokens = total_tokens(prior.usage)) {
        if (prior_tokens > std::numeric_limits<std::uint64_t>::max() - *tokens)
          return "worker token budget exceeded";
        prior_tokens += *tokens;
      }
      if (prior.usage.cost_units)
        prior_cost += *prior.usage.cost_units;
    }
  }
  if (max_tokens_per_run_ != 0)
    if (const auto tokens = total_tokens(value.usage);
        tokens &&
        (prior_tokens > max_tokens_per_run_ || *tokens > max_tokens_per_run_ - prior_tokens))
      return "worker token budget exceeded";
  if (max_cost_units_per_run_ != 0 && value.usage.cost_units &&
      prior_cost + *value.usage.cost_units > max_cost_units_per_run_)
    return "worker cost_units budget exceeded";
  return {};
}

void WorkerManager::persist(WorkerJob &value) {
  if (value.error.size() > 512)
    value.error.resize(512);
  if (value.cancellation_error.size() > 512)
    value.cancellation_error.resize(512);
  std::size_t artifact_bytes = 0;
  for (const auto &artifact : value.artifacts) {
    const auto size = artifact.dump().size();
    if (size > max_artifact_metadata_bytes || artifact_bytes > max_artifact_metadata_bytes - size)
      throw Error(ErrorCode::Validation, "Worker artifact metadata exceeds limit");
    artifact_bytes += size;
  }
  if (value.result.dump().size() > max_result_bytes ||
      value.result_metadata.dump().size() > max_metadata_bytes ||
      value.artifacts.size() > max_artifacts_)
    throw Error(ErrorCode::Validation, "Worker result exceeds limit");
  if (value.usage.metadata.dump().size() > max_usage_metadata_bytes)
    throw Error(ErrorCode::Validation, "Worker usage metadata exceeds limit");
  if (value.state != WorkerJobState::Completed)
    value.continuation.reset();
  storage_.commit({{RecordKind::WorkerJob, value.id, value.run_id, Json(value)}});
}

void WorkerManager::retire_superseded_distributed_jobs_locked() {
  for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
    auto value = record.get<WorkerJob>();
    if (worker_job_terminal(value.state))
      continue;
    const auto work_id = value.request_metadata.value("node_work_id", std::string{});
    const auto attempt_id = value.request_metadata.value("node_work_attempt_id", std::string{});
    if (work_id.empty() || attempt_id.empty())
      continue;
    try {
      const auto work = storage_.get(RecordKind::NodeWork, work_id).get<NodeWork>();
      if (work.state == NodeWorkState::Running && work.attempt_id == attempt_id)
        continue;
      value.state = WorkerJobState::Failed;
      value.failure_kind = WorkerFailureKind::Transport;
      value.error = "Worker attempt was superseded or lost its node lease";
      value.completed_at = timestamp();
      persist(value);
    } catch (const Error &) {
      // A concurrently recovered node may disappear between the list and the
      // lookup.  Leave that job for the normal durable recovery path.
    }
  }
}

void WorkerManager::fence_stale_submission_locked(WorkerJob &value) {
  if (value.state != WorkerJobState::Submitting || !value.external_job_id.empty())
    return;
  {
    std::lock_guard async_lock(async_mutex_);
    if (async_submissions_.contains(value.id) || active_submissions_.contains(value.id))
      return;
  }
  value.state = WorkerJobState::Unknown;
  value.error = "Worker submission was interrupted before its external job ID was recorded; "
                "the outcome is unknown and it will not be resubmitted";
  persist(value);
  submission_cancellation_signals_.erase(value.id);
  state_changed_.notify_all();
}

void WorkerManager::fence_stale_submissions_locked() {
  for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
    auto value = record.get<WorkerJob>();
    fence_stale_submission_locked(value);
  }
}

WorkerJob WorkerManager::finalize_pending_cancellation(const std::string &id) {
  std::lock_guard state_lock(state_mutex_);
  auto value = job(id);
  if (worker_job_terminal(value.state) || !value.cancellation_requested ||
      !value.external_job_id.empty())
    return value;
  value.state = value.cancellation_target_state;
  value.cancellation_acknowledged = true;
  value.cancellation_error.clear();
  value.completed_at = timestamp();
  persist(value);
  submission_cancellation_signals_.erase(value.id);
  state_changed_.notify_all();
  return value;
}

WorkerJob WorkerManager::record_submission(const std::string &id,
                                           const WorkerSubmission &submission,
                                           const std::shared_ptr<WorkerTransport> &adapter) {
  if (submission.external_job_id.empty() ||
      submission.external_job_id.size() > max_external_id_bytes)
    throw WorkerTransportError("Worker returned an invalid external job id");

  bool cancel_external_job = false;
  WorkerJob value;
  {
    std::lock_guard state_lock(state_mutex_);
    value = job(id);
    if (worker_job_terminal(value.state))
      return value;
    // Publish the accepted handle before releasing state_mutex_. A racing
    // cancel() then either sets cancellation_requested before this check or
    // observes this handle and cancels the exact external job itself.
    value.external_job_id = submission.external_job_id;
    value.state = submission.state;
    value.result_metadata = submission.metadata;
    apply_continuation(value, submission.continuation);
    if (!submission.result.is_null())
      value.result = submission.result;
    if (!submission.artifacts.empty())
      value.artifacts = submission.artifacts;
    if (!submission.error.empty())
      value.error = bounded_error(submission.error);
    merge_usage(value.usage, submission.usage);
    cancel_external_job = value.cancellation_requested && !worker_job_terminal(value.state);
    if (worker_job_terminal(value.state))
      value.completed_at = timestamp();
    if (const auto violation = budget_violation(value); !violation.empty()) {
      value.state = WorkerJobState::Failed;
      value.failure_kind = WorkerFailureKind::Budget;
      value.error = violation;
      value.completed_at = timestamp();
    } else if (value.state == WorkerJobState::Failed) {
      value.failure_kind = WorkerFailureKind::Job;
    }
    persist(value);
    submission_cancellation_signals_.erase(value.id);
    state_changed_.notify_all();
  }

  if (!cancel_external_job)
    return value;

  bool cancellation_acknowledged = false;
  try {
    cancellation_acknowledged = adapter->cancel(submission.external_job_id);
  } catch (...) {
  }

  std::lock_guard state_lock(state_mutex_);
  value = job(id);
  if (worker_job_terminal(value.state))
    return value;
  if (cancellation_acknowledged) {
    value.state = value.cancellation_target_state;
    value.cancellation_acknowledged = true;
    value.cancellation_error.clear();
    value.completed_at = timestamp();
  } else if (value.cancellation_error.empty()) {
    value.cancellation_error = "Worker did not acknowledge cancellation";
  }
  persist(value);
  state_changed_.notify_all();
  return value;
}

WorkerJob WorkerManager::reconcile(WorkerJob value, bool fail_transport) {
  // An asynchronous submission remains in Submitting until the provider
  // handle is available.  Do not call adapter->status() with an empty handle:
  // process-backed transports serialize status with submit, so doing so would
  // block the runtime/lease loop behind provider startup or execution.
  const auto terminal_result_missing = browser_tool_child_needs_result(value);
  if ((worker_job_terminal(value.state) && !terminal_result_missing) ||
      value.external_job_id.empty())
    return value;
  auto adapter = registry_.get(value.worker_id);
  const auto metadata = adapter->metadata();
  if (!metadata.supports_status && !metadata.supports_recovery)
    return value;
  try {
    if (terminal_result_missing) {
      const auto final_result = adapter->result(value.external_job_id);
      std::lock_guard state_lock(state_mutex_);
      value = job(value.id);
      if (browser_tool_child_needs_result(value) &&
          final_result.state == WorkerJobState::Completed && !final_result.result.is_null() &&
          final_result.result.dump().size() <= max_result_bytes) {
        value.result = final_result.result;
        value.request_metadata["codex_tool_result_retrieved"] = true;
        if (final_result.metadata.is_object() &&
            final_result.metadata.dump().size() <= max_metadata_bytes)
          value.result_metadata = final_result.metadata;
        if (!final_result.error.empty())
          value.error = bounded_error(final_result.error);
        if (!final_result.artifacts.empty() && final_result.artifacts.size() <= max_artifacts_)
          value.artifacts = final_result.artifacts;
        merge_usage(value.usage, final_result.usage);
        persist(value);
      }
      return value;
    }
    auto status = adapter->status(value.external_job_id);
    bool final_result_received = false;
    // Several supervised workers acknowledge terminal state separately from
    // their final payload. Read the result only after status says Completed;
    // the terminal result is then committed with this durable WorkerJob.
    const bool is_codex_tool_child = value.request_metadata.contains("parent_worker_job_id");
    if (status.state == WorkerJobState::Completed &&
        (status.result.is_null() || is_codex_tool_child)) {
      const auto final_result = adapter->result(value.external_job_id);
      if (final_result.state == WorkerJobState::Completed && !final_result.result.is_null()) {
        status.result = final_result.result;
        final_result_received = true;
      }
      if (final_result.metadata.is_object() && !final_result.metadata.empty()) {
        if (!status.metadata.is_object())
          status.metadata = Json::object();
        status.metadata.update(final_result.metadata);
      }
      if (!final_result.error.empty())
        status.error = final_result.error;
      if (!final_result.artifacts.empty())
        status.artifacts = final_result.artifacts;
      merge_usage(status.usage, final_result.usage);
      apply_continuation(value, final_result.continuation);
    }
    if (status.state == WorkerJobState::Unknown) {
      std::lock_guard state_lock(state_mutex_);
      value = job(value.id);
      if (!worker_job_terminal(value.state) && value.state != WorkerJobState::Unknown) {
        value.state = WorkerJobState::Unknown;
        value.error = "Worker process no longer knows this job; it will not be resubmitted";
        persist(value);
      }
      return value;
    }
    std::lock_guard state_lock(state_mutex_);
    value = job(value.id);
    if (worker_job_terminal(value.state))
      return value;
    if (!valid_worker_job_transition(value.state, status.state) && value.state != status.state)
      return value;
    if (status.state != value.state)
      value.state = status.state;
    if (!status.result.is_null() && status.result.dump().size() <= max_result_bytes)
      value.result = status.result;
    if (status.metadata.is_object() && status.metadata.dump().size() <= max_metadata_bytes)
      value.result_metadata = status.metadata;
    apply_continuation(value, status.continuation);
    merge_usage(value.usage, status.usage);
    if (!status.artifacts.empty() && status.artifacts.size() <= max_artifacts_)
      value.artifacts = status.artifacts;
    if (!status.error.empty())
      value.error = bounded_error(status.error);
    if (const auto violation = budget_violation(value); !violation.empty()) {
      value.state = WorkerJobState::Failed;
      value.failure_kind = WorkerFailureKind::Budget;
      value.error = violation;
    } else if (value.state == WorkerJobState::Failed &&
               value.failure_kind == WorkerFailureKind::None) {
      value.failure_kind = WorkerFailureKind::Job;
    }
    if (worker_job_terminal(value.state) && value.completed_at.empty())
      value.completed_at = timestamp();
    if (status.state == WorkerJobState::Completed && final_result_received &&
        value.request_metadata.contains("parent_worker_job_id"))
      value.request_metadata["codex_tool_result_retrieved"] = true;
    persist(value);
  } catch (const WorkerTransportError &error) {
    if (fail_transport) {
      std::lock_guard state_lock(state_mutex_);
      value = job(value.id);
      if (!worker_job_terminal(value.state)) {
        value.state = WorkerJobState::Failed;
        value.failure_kind = WorkerFailureKind::Transport;
        value.error = bounded_error(error.what());
        value.completed_at = timestamp();
        persist(value);
      }
    }
  } catch (const Error &) { // NOLINT(bugprone-empty-catch): recovery is best effort.
    // Recovery is deliberately conservative. An adapter failure does not
    // invent a terminal result or submit a second external task.
  } catch (...) { // NOLINT(bugprone-empty-catch): contain native adapter failures.
    // Native adapter exceptions are contained by the loader; this is an
    // additional safety boundary around recovery.
  }
  return value;
}

std::shared_ptr<std::mutex> WorkerManager::submit_mutex_for(const std::string &worker_id) {
  std::lock_guard lock(submit_mutexes_mutex_);
  auto &mutex = submit_mutexes_[worker_id];
  if (!mutex)
    mutex = std::make_shared<std::mutex>();
  return mutex;
}

WorkerJob WorkerManager::submit(const WorkerRequest &request) {
  return submit_impl(request, false);
}

WorkerJob WorkerManager::submit_impl(const WorkerRequest &request, bool asynchronous_dispatch,
                                     const std::string &initial_submission_id) {
  if (stopped_)
    throw Error(ErrorCode::Conflict, "Worker manager is stopped");
  if (request.idempotency_key.empty() || request.idempotency_key.size() > 512 ||
      request.task_type.size() > 128 || request.capability.size() > 128 ||
      request.parent_worker_job_id.size() > max_job_id_bytes ||
      request.parent_tool_call_id.size() > process_protocol::max_interaction_id_bytes ||
      request.parent_tool_turn_id.size() > process_protocol::max_interaction_id_bytes ||
      request.parent_provider_session_id.size() > process_protocol::max_interaction_id_bytes ||
      request.instructions.size() > max_result_bytes ||
      request.input.dump().size() > max_result_bytes ||
      request.metadata.dump().size() > max_metadata_bytes ||
      request.artifact_ids.size() > max_artifacts_)
    throw Error(ErrorCode::Validation, "Worker request exceeds limit");
  if ((request.session_context &&
       (!request.durable_session ||
        request.session_context->payload.dump().size() > max_result_bytes ||
        request.session_context->recent_turns.dump().size() > max_result_bytes)) ||
      (request.continuation &&
       (request.continuation->provider_id.empty() ||
        request.continuation->provider_id.size() > 256 ||
        request.continuation->provider_version.empty() ||
        request.continuation->provider_version.size() > 128 ||
        request.continuation->state.empty() || request.continuation->state.size() > 64 * 1024)))
    throw Error(ErrorCode::Validation, "Worker session context exceeds limit");
  for (const auto &artifact : request.artifact_ids)
    if (!bounded_identifier(artifact, 128))
      throw Error(ErrorCode::Validation, "Invalid worker artifact reference");

  auto worker_id = resolve_worker(request.worker_id, request.capability);
  const auto worker_submit_mutex = submit_mutex_for(worker_id);
  std::lock_guard submit_lock(*worker_submit_mutex);
  const auto durable_id = durable_job_id(request.idempotency_key);
  {
    std::lock_guard async_lock(async_mutex_);
    active_submissions_.insert(durable_id);
  }
  struct ActiveSubmissionGuard {
    std::mutex &mutex;
    std::set<std::string> &active_submissions;
    const std::string &job_id;
    ~ActiveSubmissionGuard() {
      std::lock_guard active_lock(mutex);
      active_submissions.erase(job_id);
    }
  } active_submission_guard{async_mutex_, active_submissions_, durable_id};
  auto adapter = registry_.get(worker_id);
  auto existing_job = [&]() -> std::optional<WorkerJob> {
    std::lock_guard state_lock(state_mutex_);
    try {
      auto existing = job(durable_id);
      if (existing.idempotency_key != request.idempotency_key)
        throw Error(ErrorCode::Conflict, "Worker idempotency key collision");
      if (existing.worker_id != worker_id)
        throw Error(ErrorCode::Conflict, "Worker idempotency key belongs to another worker");
      return existing;
    } catch (const Error &error) {
      if (error.code == ErrorCode::NotFound)
        return std::nullopt;
      throw;
    }
  };
  auto existing = existing_job();
  const bool initial_dispatch = !initial_submission_id.empty();
  if (initial_dispatch &&
      (!existing.has_value() || existing->id != initial_submission_id ||
       existing->state != WorkerJobState::Submitting || !existing->external_job_id.empty()))
    throw Error(ErrorCode::Conflict, "Initial worker dispatch no longer owns its durable job");
  if (existing.has_value() && !initial_dispatch) {
    if (!worker_id.empty() && existing->worker_id != worker_id)
      throw Error(ErrorCode::Conflict, "Worker idempotency key belongs to another worker");
    if (worker_job_terminal(existing->state) || existing->state == WorkerJobState::Unknown)
      return *existing;
    if (existing->state == WorkerJobState::Submitting && existing->external_job_id.empty()) {
      bool dispatching = false;
      {
        std::lock_guard async_lock(async_mutex_);
        dispatching = async_submissions_.contains(existing->id);
      }
      if (dispatching && !asynchronous_dispatch)
        return *existing;
      if (!dispatching) {
        std::lock_guard state_lock(state_mutex_);
        existing = job(existing->id);
        if (existing->state == WorkerJobState::Submitting && existing->external_job_id.empty()) {
          existing->state = WorkerJobState::Unknown;
          existing->error =
              "Worker submission outcome is unknown after restart; it will not be resubmitted";
          persist(*existing);
        }
        return *existing;
      }
    }
  }

  if (existing.has_value() && existing->cancellation_requested && existing->external_job_id.empty())
    return finalize_pending_cancellation(existing->id);

  auto metadata = adapter->metadata();
  if ((metadata.supports_recovery || adapter->supports_transport_restart()) && !metadata.healthy &&
      metadata.status != "disabled" && metadata.status != "unavailable") {
    try {
      // A supervised transport may have torn down its process group while a
      // previous submission was timing out.  Re-establish ownership before
      // checking availability for a retry; start() is serialized by the
      // transport and is idempotent for healthy adapters.
      adapter->start();
      metadata = adapter->metadata();
    } catch (...) {
    }
  }
  const auto recoverable = metadata.status == "failed" &&
                           (metadata.supports_recovery || adapter->supports_transport_restart());
  if (!metadata.enabled || metadata.status == "disabled" || metadata.status == "unavailable" ||
      (!metadata.healthy && !recoverable))
    throw Error(ErrorCode::Capacity, "Worker is unavailable");

  if (existing.has_value() && !initial_dispatch) {
    if (!existing->external_job_id.empty())
      return reconcile(std::move(*existing), false);
    if (!metadata.supports_recovery) {
      std::lock_guard state_lock(state_mutex_);
      existing = job(durable_id);
      existing->state = WorkerJobState::Unknown;
      existing->error = "External submission outcome is unknown; adapter recovery is unavailable";
      persist(*existing);
      return *existing;
    }
    auto retry_request = request;
    retry_request.job_id = existing->id;
    {
      std::lock_guard state_lock(state_mutex_);
      if (const auto signal = submission_cancellation_signals_.find(existing->id);
          signal != submission_cancellation_signals_.end())
        retry_request.cancellation_signal = signal->second;
    }
    try {
      const auto submission = adapter->submit(retry_request);
      return record_submission(durable_id, submission, adapter);
    } catch (const WorkerSubmissionCancelled &) {
      return finalize_pending_cancellation(durable_id);
    } catch (const WorkerTransportError &error) {
      {
        std::lock_guard state_lock(state_mutex_);
        existing = job(durable_id);
        if (!worker_job_terminal(existing->state)) {
          if (!existing->cancellation_requested) {
            existing->state = error.timed_out ? WorkerJobState::TimedOut : WorkerJobState::Failed;
            existing->failure_kind = WorkerFailureKind::Transport;
            existing->error = bounded_error(error.what());
            existing->completed_at = timestamp();
            persist(*existing);
          }
        }
      }
      if (existing->cancellation_requested && !worker_job_terminal(existing->state)) {
        std::unique_lock wait_lock(state_mutex_);
        state_changed_.wait_for(wait_lock, std::chrono::seconds(1), [&] {
          try {
            *existing = job(durable_id);
            return worker_job_terminal(existing->state);
          } catch (...) {
            return false;
          }
        });
      }
      return *existing;
    } catch (...) {
      std::lock_guard state_lock(state_mutex_);
      existing = job(durable_id);
      if (!worker_job_terminal(existing->state)) {
        existing->state = WorkerJobState::Failed;
        existing->failure_kind = WorkerFailureKind::Job;
        existing->error = "Worker retry submission failed";
        existing->completed_at = timestamp();
        persist(*existing);
      }
      return *existing;
    }
  }

  WorkerJob created;
  if (initial_dispatch) {
    created = *existing;
  } else {
    if (metadata.supports_status) {
      std::vector<WorkerJob> active_worker_jobs;
      {
        std::lock_guard state_lock(state_mutex_);
        for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
          auto active = record.get<WorkerJob>();
          if (active.worker_id == worker_id && !worker_job_terminal(active.state))
            active_worker_jobs.push_back(std::move(active));
        }
      }
      if (active_worker_jobs.size() >= max_per_worker_)
        for (auto &active : active_worker_jobs)
          (void)reconcile(std::move(active), false);
    }
    std::lock_guard state_lock(state_mutex_);
    retire_superseded_distributed_jobs_locked();
    fence_stale_submissions_locked();
    std::size_t active = 0, worker_active = 0;
    for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
      const auto stored = record.get<WorkerJob>();
      // Unknown outcomes stay durable and non-retriable without reserving a
      // live slot, so an unresolved worker cannot block unrelated capacity.
      if (worker_job_terminal(stored.state) || stored.state == WorkerJobState::Unknown)
        continue;
      ++active;
      if (stored.worker_id == worker_id)
        ++worker_active;
    }
    if (active >= max_active_)
      throw Error(ErrorCode::Capacity, "Worker job capacity is exhausted");
    if (worker_active >= max_per_worker_)
      throw Error(ErrorCode::Capacity, "Worker-specific job capacity is exhausted");

    created.id = durable_id;
    created.worker_id = worker_id;
    created.run_id = request.run_id;
    created.node_id = request.node_id;
    created.attempt = request.attempt;
    created.idempotency_key = request.idempotency_key;
    created.request_metadata = {{"task_type", request.task_type},
                                {"capability", request.capability},
                                {"deadline", request.deadline},
                                {"artifact_ids", request.artifact_ids}};
    if (!request.parent_worker_job_id.empty())
      created.request_metadata["parent_worker_job_id"] = request.parent_worker_job_id;
    if (!request.parent_tool_call_id.empty())
      created.request_metadata["parent_tool_call_id"] = request.parent_tool_call_id;
    if (!request.parent_tool_turn_id.empty())
      created.request_metadata["parent_codex_turn_id"] = request.parent_tool_turn_id;
    if (!request.parent_provider_session_id.empty())
      created.request_metadata["parent_codex_session_id"] = request.parent_provider_session_id;
    if (request.metadata.is_object()) {
      for (const auto &key :
           {"classification", "node_work_id", "node_work_attempt_id", "required_tool"})
        if (request.metadata.contains(key))
          created.request_metadata[key] = request.metadata.at(key);
    }
    if (!storage_.claim({RecordKind::WorkerJob, created.id, created.run_id, Json(created)}))
      return job(durable_id);
    created.state = WorkerJobState::Submitting;
    persist(created);
    submission_cancellation_signals_[created.id] = std::make_shared<std::atomic<bool>>(false);
  }

  auto pending_cancellation = finalize_pending_cancellation(created.id);
  if (worker_job_terminal(pending_cancellation.state))
    return pending_cancellation;

  try {
    auto outbound = request;
    outbound.job_id = created.id;
    {
      std::lock_guard state_lock(state_mutex_);
      if (const auto signal = submission_cancellation_signals_.find(created.id);
          signal != submission_cancellation_signals_.end())
        outbound.cancellation_signal = signal->second;
    }
    const auto submission = adapter->submit(outbound);
    created = record_submission(created.id, submission, adapter);
  } catch (const WorkerSubmissionCancelled &) {
    created = finalize_pending_cancellation(created.id);
  } catch (const WorkerTransportError &error) {
    {
      std::lock_guard state_lock(state_mutex_);
      created = job(created.id);
      if (!worker_job_terminal(created.state)) {
        if (!created.cancellation_requested) {
          created.state = error.timed_out ? WorkerJobState::TimedOut : WorkerJobState::Failed;
          created.failure_kind = WorkerFailureKind::Transport;
          created.error = bounded_error(error.what());
          created.completed_at = timestamp();
          log_diagnostic("worker.transport_failed",
                         {{"worker_job_id", created.id}, {"worker_id", created.worker_id}});
          persist(created);
        }
      }
    }
    if (created.cancellation_requested && !worker_job_terminal(created.state)) {
      std::unique_lock wait_lock(state_mutex_);
      state_changed_.wait_for(wait_lock, std::chrono::seconds(1), [&] {
        try {
          created = job(created.id);
          return worker_job_terminal(created.state);
        } catch (...) {
          return false;
        }
      });
    }
  } catch (const Error &error) {
    if (error.code == ErrorCode::Storage) {
      // Leave the durable submission recoverable. The runtime lease/recovery
      // path will fence this attempt if the outage outlives its lease.
      log_diagnostic("worker.submission_deferred_after_storage_error",
                     {{"worker_job_id", created.id}, {"worker_id", created.worker_id}});
      throw;
    }
    std::lock_guard state_lock(state_mutex_);
    created = job(created.id);
    if (!worker_job_terminal(created.state)) {
      created.state = WorkerJobState::Failed;
      created.failure_kind = WorkerFailureKind::Job;
      created.error = bounded_error(error.code == ErrorCode::Plugin ? error.what()
                                                                    : "Worker submission failed");
      created.completed_at = timestamp();
      log_diagnostic("worker.submission_failed", {{"worker_job_id", created.id},
                                                  {"worker_id", created.worker_id},
                                                  {"reason", created.error}});
      persist(created);
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    created = job(created.id);
    if (!worker_job_terminal(created.state)) {
      created.state = WorkerJobState::Failed;
      created.failure_kind = WorkerFailureKind::Job;
      created.error = "Worker submission failed";
      created.completed_at = timestamp();
      log_diagnostic("worker.submission_failed",
                     {{"worker_job_id", created.id}, {"worker_id", created.worker_id}});
      persist(created);
    }
  }
  return job(created.id);
}

WorkerJob WorkerManager::submit_async(const WorkerRequest &request) {
  if (stopped_)
    throw Error(ErrorCode::Conflict, "Worker manager is stopped");
  if (request.idempotency_key.empty() || request.idempotency_key.size() > 512 ||
      request.task_type.size() > 128 || request.capability.size() > 128 ||
      request.parent_worker_job_id.size() > max_job_id_bytes ||
      request.parent_tool_call_id.size() > process_protocol::max_interaction_id_bytes ||
      request.parent_tool_turn_id.size() > process_protocol::max_interaction_id_bytes ||
      request.parent_provider_session_id.size() > process_protocol::max_interaction_id_bytes ||
      request.instructions.size() > max_result_bytes ||
      request.input.dump().size() > max_result_bytes ||
      request.metadata.dump().size() > max_metadata_bytes ||
      request.artifact_ids.size() > max_artifacts_)
    throw Error(ErrorCode::Validation, "Worker request exceeds limit");
  if ((request.session_context &&
       (!request.durable_session ||
        request.session_context->payload.dump().size() > max_result_bytes ||
        request.session_context->recent_turns.dump().size() > max_result_bytes)) ||
      (request.continuation &&
       (request.continuation->provider_id.empty() ||
        request.continuation->provider_id.size() > 256 ||
        request.continuation->provider_version.empty() ||
        request.continuation->provider_version.size() > 128 ||
        request.continuation->state.empty() || request.continuation->state.size() > 64 * 1024)))
    throw Error(ErrorCode::Validation, "Worker session context exceeds limit");
  for (const auto &artifact : request.artifact_ids)
    if (!bounded_identifier(artifact, 128))
      throw Error(ErrorCode::Validation, "Invalid worker artifact reference");

  const auto worker_id = resolve_worker(request.worker_id, request.capability);
  const auto worker_submit_mutex = submit_mutex_for(worker_id);
  std::lock_guard submit_lock(*worker_submit_mutex);
  const auto adapter = registry_.get(worker_id);
  auto metadata = adapter->metadata();
  if ((metadata.supports_recovery || adapter->supports_transport_restart()) && !metadata.healthy &&
      metadata.status != "disabled" && metadata.status != "unavailable") {
    try {
      adapter->start();
      metadata = adapter->metadata();
    } catch (...) {
    }
  }
  const auto recoverable = metadata.status == "failed" &&
                           (metadata.supports_recovery || adapter->supports_transport_restart());
  if (!metadata.enabled || metadata.status == "disabled" || metadata.status == "unavailable" ||
      (!metadata.healthy && !recoverable))
    throw Error(ErrorCode::Capacity, "Worker is unavailable");

  // Remote workers that advertise status but cannot recover submissions may
  // have durable handles left behind after their endpoint disconnects. Probe
  // those handles before applying this worker's capacity limit so a terminal
  // or unknown remote job cannot reserve a slot forever. Keep these calls out
  // of state_mutex_: status may cross a network boundary. No old job is ever
  // resubmitted here.
  if (metadata.remote && metadata.supports_status && !metadata.supports_recovery) {
    std::vector<WorkerJob> active_remote_jobs;
    {
      std::lock_guard state_lock(state_mutex_);
      for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
        auto active = record.get<WorkerJob>();
        if (active.worker_id == worker_id && !worker_job_terminal(active.state) &&
            active.state != WorkerJobState::Unknown && !active.external_job_id.empty())
          active_remote_jobs.push_back(std::move(active));
      }
    }
    for (auto &active : active_remote_jobs)
      (void)reconcile(std::move(active), true);
  }

  const auto durable_id = durable_job_id(request.idempotency_key);
  WorkerJob created;
  bool dispatch_reserved = false;
  {
    std::lock_guard state_lock(state_mutex_);
    retire_superseded_distributed_jobs_locked();
    fence_stale_submissions_locked();
    try {
      created = job(durable_id);
      if (created.idempotency_key != request.idempotency_key)
        throw Error(ErrorCode::Conflict, "Worker idempotency key collision");
      if (created.worker_id != worker_id)
        throw Error(ErrorCode::Conflict, "Worker idempotency key belongs to another worker");
      if (worker_job_terminal(created.state) || created.state == WorkerJobState::Unknown ||
          !created.external_job_id.empty())
        return created;
      if (created.state == WorkerJobState::Submitting) {
        bool dispatching = false;
        {
          std::lock_guard async_lock(async_mutex_);
          dispatching = async_submissions_.contains(created.id);
        }
        if (!dispatching) {
          created.state = WorkerJobState::Unknown;
          created.error =
              "Worker submission outcome is unknown after restart; it will not be resubmitted";
          persist(created);
          return created;
        }
      }
    } catch (const Error &error) {
      if (error.code != ErrorCode::NotFound)
        throw;
      std::size_t active = 0, worker_active = 0;
      for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
        const auto stored = record.get<WorkerJob>();
        // Unknown outcomes stay durable and non-retriable without reserving a
        // live slot, so an unresolved worker cannot block unrelated capacity.
        if (worker_job_terminal(stored.state) || stored.state == WorkerJobState::Unknown)
          continue;
        ++active;
        if (stored.worker_id == worker_id)
          ++worker_active;
      }
      if (active >= max_active_)
        throw Error(ErrorCode::Capacity, "Worker job capacity is exhausted");
      if (worker_active >= max_per_worker_)
        throw Error(ErrorCode::Capacity, "Worker-specific job capacity is exhausted");

      created.id = durable_id;
      created.worker_id = worker_id;
      created.run_id = request.run_id;
      created.node_id = request.node_id;
      created.attempt = request.attempt;
      created.idempotency_key = request.idempotency_key;
      created.request_metadata = {{"task_type", request.task_type},
                                  {"capability", request.capability},
                                  {"deadline", request.deadline},
                                  {"artifact_ids", request.artifact_ids}};
      if (!request.parent_worker_job_id.empty())
        created.request_metadata["parent_worker_job_id"] = request.parent_worker_job_id;
      if (!request.parent_tool_call_id.empty())
        created.request_metadata["parent_tool_call_id"] = request.parent_tool_call_id;
      if (!request.parent_tool_turn_id.empty())
        created.request_metadata["parent_codex_turn_id"] = request.parent_tool_turn_id;
      if (!request.parent_provider_session_id.empty())
        created.request_metadata["parent_codex_session_id"] = request.parent_provider_session_id;
      if (request.metadata.is_object()) {
        for (const auto &key :
             {"classification", "node_work_id", "node_work_attempt_id", "required_tool"})
          if (request.metadata.contains(key))
            created.request_metadata[key] = request.metadata.at(key);
      }
      if (!storage_.claim({RecordKind::WorkerJob, created.id, created.run_id, Json(created)}))
        return job(durable_id);
      created.state = WorkerJobState::Submitting;
      persist(created);
      submission_cancellation_signals_[created.id] = std::make_shared<std::atomic<bool>>(false);
      {
        std::lock_guard async_lock(async_mutex_);
        dispatch_reserved = async_submissions_.insert(created.id).second;
      }
    }
  }

  {
    std::lock_guard async_lock(async_mutex_);
    if (!dispatch_reserved && !async_submissions_.insert(created.id).second)
      return created;
    async_threads_.emplace_back([this, request, id = created.id](std::stop_token) {
      try {
        (void)submit_impl(request, true, id);
      } catch (const Error &error) {
        if (error.code == ErrorCode::Storage) {
          log_diagnostic("worker.submission_deferred_after_storage_error",
                         {{"worker_job_id", id}, {"worker_id", request.worker_id}});
        } else {
          try {
            std::lock_guard state_lock(state_mutex_);
            auto failed = job(id);
            if (!worker_job_terminal(failed.state)) {
              failed.state = WorkerJobState::Failed;
              failed.failure_kind = WorkerFailureKind::Job;
              failed.error = bounded_error(error.what());
              failed.completed_at = timestamp();
              persist(failed);
            }
          } catch (...) {
          }
        }
      } catch (...) {
        try {
          std::lock_guard state_lock(state_mutex_);
          auto failed = job(id);
          if (!worker_job_terminal(failed.state)) {
            failed.state = WorkerJobState::Failed;
            failed.failure_kind = WorkerFailureKind::Job;
            failed.error = "Worker submission failed";
            failed.completed_at = timestamp();
            persist(failed);
          }
        } catch (...) {
        }
      }
      {
        std::lock_guard async_lock(async_mutex_);
        async_submissions_.erase(id);
      }
      {
        std::lock_guard state_lock(state_mutex_);
        submission_cancellation_signals_.erase(id);
      }
    });
  }
  return created;
}

void WorkerManager::cancel(const std::string &id, WorkerJobState requested_state,
                           const std::string &reason) {
  std::string worker_id;
  std::string external_job_id;
  bool pending_submission = false;
  {
    std::lock_guard state_lock(state_mutex_);
    auto value = job(id);
    if (worker_job_terminal(value.state))
      return;
    value.cancellation_requested = true;
    value.cancellation_target_state = requested_state;
    value.cancellation_error = bounded_error(reason);
    if (const auto signal = submission_cancellation_signals_.find(id);
        signal != submission_cancellation_signals_.end())
      signal->second->store(true, std::memory_order_release);
    if (value.external_job_id.empty()) {
      pending_submission = true;
      worker_id = value.worker_id;
      persist(value);
    } else {
      worker_id = value.worker_id;
      external_job_id = value.external_job_id;
      persist(value);
    }
  }

  bool acknowledged = false;
  std::string cancellation_error;
  try {
    const auto adapter = registry_.get(worker_id);
    if (pending_submission) {
      acknowledged = adapter->cancel_pending(id);
      if (acknowledged) {
        // cancel_pending may terminate a process group while the transport's
        // submit call still owns its mutex.  Restarting inline would block the
        // LASO execution/lease loop behind that in-flight call.  Queue the
        // bounded restart instead; the next submission also performs the
        // normal transport health check before it uses the adapter.
        std::lock_guard async_lock(async_mutex_);
        async_threads_.emplace_back([adapter](std::stop_token) {
          try {
            adapter->start();
          } catch (...) {
          }
        });
      }
    } else {
      acknowledged = adapter->cancel(external_job_id);
    }
  } catch (...) {
    cancellation_error =
        pending_submission ? "Worker pending cancellation failed" : "Worker cancellation failed";
  }

  {
    std::lock_guard state_lock(state_mutex_);
    auto value = job(id);
    if (worker_job_terminal(value.state))
      return;
    {
      std::lock_guard lock(interaction_mutex_);
      for (const auto &record : storage_.list(RecordKind::WorkerInteraction, "", 10000, 0)) {
        auto interaction = record.get<WorkerInteraction>();
        if (interaction.worker_job_id == value.id &&
            interaction.state == WorkerInteractionState::Pending) {
          interaction.state = WorkerInteractionState::Cancelled;
          interaction.reason = bounded_error(reason);
          interaction.decided_at = timestamp();
          interaction.response = Json{{"decision", "cancelled"}, {"reason", interaction.reason}};
          storage_.commit({{RecordKind::WorkerInteraction, interaction.id, interaction.run_id,
                            Json(interaction)}});
        }
      }
    }
    interaction_changed_.notify_all();
    if (requested_state == WorkerJobState::TimedOut) {
      if (acknowledged) {
        value.cancellation_acknowledged = true;
        value.state = WorkerJobState::TimedOut;
        value.cancellation_error.clear();
        value.completed_at = timestamp();
      } else {
        value.cancellation_error = cancellation_error.empty()
                                       ? "Worker did not acknowledge timeout cancellation"
                                       : cancellation_error;
      }
    } else if (acknowledged) {
      value.cancellation_acknowledged = true;
      value.state = WorkerJobState::Cancelled;
      value.cancellation_error.clear();
      value.completed_at = timestamp();
    } else {
      value.cancellation_error = cancellation_error.empty()
                                     ? "Worker did not acknowledge cancellation"
                                     : cancellation_error;
    }
    persist(value);
    state_changed_.notify_all();
  }

  // Child Computer jobs are durable records with a stable parent reference.
  // Scan and cancel after releasing the manager state lock so remote transport
  // cancellation never runs under a global state lock.
  for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
    const auto child = record.get<WorkerJob>();
    if (child.request_metadata.value("parent_worker_job_id", std::string{}) == id &&
        !worker_job_terminal(child.state))
      cancel(child.id, requested_state, reason);
  }
}

namespace {
bool interaction_terminal(WorkerInteractionState state) {
  return state != WorkerInteractionState::Pending;
}

WorkerInteractionResponse interaction_response(const WorkerInteraction &interaction) {
  WorkerInteractionResponse result;
  result.request_id = interaction.id;
  result.state = interaction.state;
  result.payload = interaction.response.value("payload", Json::object());
  result.reason = interaction.reason;
  return result;
}
} // namespace

WorkerInteractionResponse
WorkerManager::handle_interaction(const WorkerInteractionRequest &request) {
  if (stopped_)
    throw WorkerTransportError("LASO worker interaction service is stopped");
  if (request.request_id.empty() ||
      request.request_id.size() > process_protocol::max_interaction_id_bytes ||
      request.worker_job_id.empty() ||
      request.worker_job_id.size() > process_protocol::max_interaction_id_bytes ||
      request.worker_id.size() > process_protocol::max_interaction_id_bytes ||
      request.external_job_id.size() > process_protocol::max_interaction_id_bytes ||
      request.session_id.size() > process_protocol::max_interaction_id_bytes ||
      request.title.size() > process_protocol::max_interaction_text_bytes ||
      request.summary.size() > process_protocol::max_interaction_text_bytes ||
      request.created_at.size() > 64 || request.deadline.size() > 64 || request.risk.size() > 128 ||
      request.category.size() > 128 || !request.payload.is_object() ||
      request.payload.dump().size() > process_protocol::max_interaction_payload_bytes)
    throw WorkerTransportError("Worker interaction request exceeds its limits");

  std::unique_lock lock(interaction_mutex_);
  WorkerInteraction interaction;
  bool existing = true;
  try {
    interaction =
        storage_.get(RecordKind::WorkerInteraction, request.request_id).get<WorkerInteraction>();
    if (interaction.worker_job_id != request.worker_job_id || interaction.type != request.type)
      throw WorkerTransportError("Worker interaction id was reused with different content");
  } catch (const Error &error) {
    if (error.code != ErrorCode::NotFound)
      throw;
    existing = false;
  }
  if (!existing) {
    interaction.id = request.request_id;
    interaction.worker_job_id = request.worker_job_id;
    interaction.worker_id = request.worker_id;
    try {
      interaction.run_id =
          storage_.get(RecordKind::WorkerJob, request.worker_job_id).value("run_id", std::string{});
    } catch (const Error &error) {
      if (error.code != ErrorCode::NotFound)
        throw;
    }
    interaction.external_job_id = request.external_job_id;
    interaction.session_id = request.session_id;
    interaction.type = request.type;
    interaction.title = request.title;
    interaction.summary = request.summary;
    interaction.payload = request.payload;
    interaction.created_at = request.created_at.empty() ? timestamp() : request.created_at;
    interaction.deadline = request.deadline;
    interaction.risk = request.risk;
    interaction.category = request.category;

    PolicyResult policy_result{PolicyDecision::RequireApproval, "Human decision required"};
    if (request.type == WorkerInteractionType::Question) {
      policy_result = {PolicyDecision::RequireApproval, "Human answer required"};
    } else if (policy_) {
      const auto resource = request.payload.value("resource", std::string{"worker.permission"});
      policy_result = policy_->evaluate(
          {"", "", resource, request.payload.value("classification", std::string{"public"}),
           request.payload.value("network", false), request.payload.value("remote", false),
           request.type == WorkerInteractionType::Approval});
    }
    if (policy_result.decision == PolicyDecision::Allow) {
      interaction.state = request.type == WorkerInteractionType::Question
                              ? WorkerInteractionState::Answered
                              : WorkerInteractionState::Approved;
      interaction.reason = policy_result.reason;
      interaction.response = Json{{"decision", interaction.state}, {"reason", interaction.reason}};
      interaction.decided_at = timestamp();
    } else if (policy_result.decision == PolicyDecision::Deny) {
      interaction.state = WorkerInteractionState::Denied;
      interaction.reason = policy_result.reason;
      interaction.response = Json{{"decision", "denied"}, {"reason", interaction.reason}};
      interaction.decided_at = timestamp();
    }
    storage_.commit(
        {{RecordKind::WorkerInteraction, interaction.id, interaction.run_id, Json(interaction)}});
  }
  if (interaction_terminal(interaction.state))
    return interaction_response(interaction);

  // timestamp() is fixed-width UTC, so this comparison is valid for values
  // produced by LASO and avoids silently waiting past an already-expired
  // request. The in-memory wait remains bounded even for malformed clocks.
  if (!interaction.deadline.empty() && interaction.deadline <= timestamp()) {
    interaction.state = WorkerInteractionState::Expired;
    interaction.reason = "Worker interaction deadline expired";
    interaction.response = Json{{"decision", "expired"}, {"reason", interaction.reason}};
    interaction.decided_at = timestamp();
    storage_.commit(
        {{RecordKind::WorkerInteraction, interaction.id, interaction.run_id, Json(interaction)}});
    return interaction_response(interaction);
  }

  const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (!interaction_changed_.wait_until(lock, wait_deadline, [&] {
    try {
      interaction =
          storage_.get(RecordKind::WorkerInteraction, request.request_id).get<WorkerInteraction>();
      return interaction_terminal(interaction.state);
    } catch (...) {
      return false;
    }
  })) {
    interaction =
        storage_.get(RecordKind::WorkerInteraction, request.request_id).get<WorkerInteraction>();
    if (!interaction_terminal(interaction.state)) {
      interaction.state = WorkerInteractionState::Expired;
      interaction.reason = "Worker interaction deadline expired";
      interaction.response = Json{{"decision", "expired"}, {"reason", interaction.reason}};
      interaction.decided_at = timestamp();
      storage_.commit(
          {{RecordKind::WorkerInteraction, interaction.id, interaction.run_id, Json(interaction)}});
    }
    break;
  }
  return interaction_response(interaction);
}

WorkerToolCallResponse WorkerManager::handle_tool_call(const WorkerToolCallRequest &request) {
  const auto failure = [&](std::string message) {
    return WorkerToolCallResponse{request.request_id, false, Json::object(),
                                  bounded_error(message)};
  };
  if (stopped_)
    return failure("LASO worker service is stopped");
  if (!bounded_identifier(request.worker_job_id, max_job_id_bytes) ||
      !bounded_identifier(request.request_id, process_protocol::max_interaction_id_bytes) ||
      !bounded_identifier(request.worker_id, process_protocol::max_interaction_id_bytes) ||
      request.session_id.empty() ||
      request.session_id.size() > process_protocol::max_interaction_id_bytes ||
      request.turn_id.empty() ||
      request.turn_id.size() > process_protocol::max_interaction_id_bytes ||
      request.deadline.size() > 64 || !request.arguments.is_object() ||
      request.arguments != Json::object())
    return failure("Invalid LASO browser status request");
  if (request.namespace_name != "laso" || request.tool != "browser_status")
    return failure("Unsupported LASO tool");

  WorkerJob parent;
  try {
    parent = job(request.worker_job_id);
  } catch (const Error &) {
    return failure("Parent worker job was not found");
  }
  if (parent.worker_id != request.worker_id || worker_job_terminal(parent.state) ||
      parent.cancellation_requested)
    return failure("Parent worker job is no longer active");
  if (parent.request_metadata.value("required_tool", std::string{}) != "laso.browser_status")
    return failure("LASO browser status is not authorized for this worker job");
  WorkerMetadata parent_worker;
  try {
    parent_worker = registry_.get(parent.worker_id)->metadata();
  } catch (const Error &) {
    return failure("Parent Codex worker is not configured");
  }
  if (parent.worker_id == "windows_computer" || parent_worker.version != "app-server" ||
      std::find(parent_worker.capabilities.begin(), parent_worker.capabilities.end(),
                "coding-agent") == parent_worker.capabilities.end())
    return failure("Only a Codex coding-agent turn may request browser status");

  constexpr const char *computer_id = "windows_computer";
  std::shared_ptr<WorkerTransport> computer_adapter;
  WorkerMetadata computer;
  try {
    computer_adapter = registry_.get(computer_id);
    computer = computer_adapter->metadata();
  } catch (const Error &) {
    return failure("Windows Computer worker is not configured");
  }
  const auto provides_browser_status =
      std::find(computer.capabilities.begin(), computer.capabilities.end(), "browser.status") !=
      computer.capabilities.end();
  if (!computer.enabled || !computer.supports_status || !computer.supports_cancellation ||
      !provides_browser_status)
    return failure("Windows Computer browser status is unavailable");
  if (!policy_)
    return failure("LASO policy is unavailable");
  const auto classification =
      parent.request_metadata.value("classification", std::string{"public"});
  const auto authorize_computer = [&](const WorkerMetadata &metadata) {
    return policy_->evaluate({"", parent.node_id, computer_id, classification, metadata.remote,
                              metadata.remote, true});
  };
  auto decision = authorize_computer(computer);
  if (decision.decision != PolicyDecision::Allow)
    return failure("Windows Computer browser status is not allowed by policy");

  // A disconnected process transport leaves its last health snapshot failed
  // even after the remote Computer endpoint reconnects. Refresh only after the
  // parent tool authorization and policy allow, and only when this adapter can
  // restart its transport. This does not enable replay of an old Computer job.
  if (!computer.healthy && computer.status != "disabled" && computer.status != "unavailable" &&
      computer_adapter->supports_transport_restart()) {
    try {
      computer_adapter->start();
    } catch (...) {
    }
    try {
      computer = computer_adapter->metadata();
    } catch (...) {
      return failure("Windows Computer browser status is unavailable");
    }
    if (computer.enabled) {
      decision = authorize_computer(computer);
      if (decision.decision != PolicyDecision::Allow)
        return failure("Windows Computer browser status is not allowed by policy");
    }
  }
  const auto refreshed_provides_browser_status =
      std::find(computer.capabilities.begin(), computer.capabilities.end(), "browser.status") !=
      computer.capabilities.end();
  if (!computer.enabled || !computer.healthy || computer.status != "healthy" ||
      !computer.supports_status || !computer.supports_cancellation ||
      !refreshed_provides_browser_status)
    return failure("Windows Computer browser status is unavailable");

  auto deadline = request.deadline;
  if (deadline.empty())
    deadline = parent.request_metadata.value("deadline", std::string{});
  if (!deadline.empty() && deadline <= timestamp())
    return failure("Parent worker job deadline has expired");

  const auto call_key = request.turn_id + ":" + request.request_id;
  if (parent.request_metadata.contains("codex_browser_status_calls")) {
    const auto &calls = parent.request_metadata.at("codex_browser_status_calls");
    if (!calls.is_array())
      return failure("Stored browser status call evidence is invalid");
    const auto duplicate = std::any_of(calls.begin(), calls.end(), [&](const Json &entry) {
      return entry.is_object() && entry.value("call_key", std::string{}) == call_key;
    });
    if (!duplicate && !calls.empty())
      return failure("This Codex turn already used its browser status tool call");
  }
  const auto idempotency_key = "codex-browser-status:" + parent.id + ":" + call_key;
  if (idempotency_key.size() > 512)
    return failure("LASO browser status call identity exceeds its limit");

  WorkerRequest child_request;
  child_request.worker_id = computer_id;
  child_request.capability = "browser.status";
  child_request.task_type = "browser.status";
  child_request.instructions = "Read-only browser status";
  child_request.input = Json::object();
  child_request.idempotency_key = idempotency_key;
  child_request.deadline = deadline;
  child_request.timeout_ms = 10000;
  child_request.run_id = parent.run_id;
  child_request.node_id =
      parent.node_id.empty() ? "codex.browser_status" : parent.node_id + ".browser_status";
  child_request.parent_worker_job_id = parent.id;
  child_request.parent_tool_call_id = call_key;
  child_request.parent_tool_turn_id = request.turn_id;
  child_request.parent_provider_session_id = request.session_id;
  child_request.metadata = {{"classification", classification}};

  WorkerJob child;
  try {
    child = submit_async(child_request);
  } catch (const Error &error) {
    return failure(error.what());
  } catch (...) {
    return failure("Unable to dispatch Windows Computer browser status");
  }

  const auto cancel_child = [&](WorkerJobState state, const std::string &reason) {
    try {
      cancel(child.id, state, reason);
    } catch (...) {
    }
  };
  const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < wait_deadline) {
    try {
      parent = job(request.worker_job_id);
    } catch (...) {
      cancel_child(WorkerJobState::Cancelled, "Parent worker job disappeared");
      return failure("Parent worker job disappeared");
    }
    if (parent.cancellation_requested || worker_job_terminal(parent.state)) {
      cancel_child(WorkerJobState::Cancelled, "Parent Codex turn was cancelled");
      return failure("Browser status was cancelled with its parent turn");
    }
    if (!deadline.empty() && deadline <= timestamp()) {
      cancel_child(WorkerJobState::TimedOut, "Parent Codex turn deadline expired");
      return failure("Browser status exceeded the parent turn deadline");
    }

    try {
      child = refresh(child.id);
    } catch (const Error &) {
      cancel_child(WorkerJobState::Cancelled, "Unable to refresh Computer job");
      return failure("Unable to refresh Windows Computer browser status");
    }
    if (worker_job_terminal(child.state)) {
      if (child.state == WorkerJobState::Completed && valid_browser_status_result(child.result)) {
        try {
          std::lock_guard state_lock(state_mutex_);
          parent = job(request.worker_job_id);
          child = job(child.id);
          if (parent.worker_id != request.worker_id || parent.run_id != child.run_id ||
              parent.cancellation_requested || worker_job_terminal(parent.state) ||
              child.worker_id != "windows_computer" || child.state != WorkerJobState::Completed ||
              !child.request_metadata.value("codex_tool_result_retrieved", false) ||
              !valid_browser_status_result(child.result))
            return failure("Browser status result lost its parent or child correlation");
          Json attestation{{"child_worker_job_id", child.id},
                           {"tool", "laso.browser_status"},
                           {"worker_id", child.worker_id},
                           {"run_id", parent.run_id},
                           {"call_key", call_key},
                           {"session_id", request.session_id},
                           {"turn_id", request.turn_id},
                           {"result_retrieved", true}};
          if (!parent.request_metadata.contains("codex_browser_status_calls") ||
              parent.request_metadata["codex_browser_status_calls"].is_null())
            parent.request_metadata["codex_browser_status_calls"] = Json::array();
          auto &calls = parent.request_metadata["codex_browser_status_calls"];
          if (!calls.is_array() || calls.size() >= 16)
            return failure("Browser status call evidence exceeded its limit");
          const auto existing = std::find_if(calls.begin(), calls.end(), [&](const Json &entry) {
            return entry.is_object() && entry.value("call_key", std::string{}) == call_key;
          });
          if (existing != calls.end()) {
            if (*existing != attestation)
              return failure("Browser status call evidence conflicts with an earlier result");
          } else {
            calls.push_back(std::move(attestation));
          }
          persist(parent);
        } catch (...) {
          return failure("Unable to persist browser status result correlation");
        }
        return {request.request_id, true, child.result, {}};
      }
      return failure(child.error.empty() ? "Windows Computer browser status failed" : child.error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  cancel_child(WorkerJobState::TimedOut, "Windows Computer browser status deadline expired");
  return failure("Windows Computer browser status timed out");
}

std::vector<Json> WorkerManager::worker_interactions(const std::string &run_id, std::size_t limit,
                                                     std::size_t offset) const {
  return storage_.list(RecordKind::WorkerInteraction, run_id, limit, offset);
}

Json WorkerManager::worker_interaction(const std::string &id) const {
  return storage_.get(RecordKind::WorkerInteraction, id);
}

void WorkerManager::resolve_interaction(const std::string &id, WorkerInteractionState state,
                                        const Json &payload, const std::string &actor,
                                        const std::string &reason) {
  const auto supported =
      state == WorkerInteractionState::Approved || state == WorkerInteractionState::Denied ||
      state == WorkerInteractionState::Answered || state == WorkerInteractionState::Cancelled ||
      state == WorkerInteractionState::Expired;
  if (id.empty() || actor.size() > 128 || reason.size() > 2048 || !payload.is_object() ||
      payload.dump().size() > process_protocol::max_interaction_payload_bytes || !supported)
    throw Error(ErrorCode::Validation, "Invalid worker interaction decision");
  std::lock_guard lock(interaction_mutex_);
  auto interaction = storage_.get(RecordKind::WorkerInteraction, id).get<WorkerInteraction>();
  if (interaction.state != WorkerInteractionState::Pending)
    throw Error(ErrorCode::Conflict, "Worker interaction is no longer pending");
  if (interaction.type != WorkerInteractionType::Question &&
      state == WorkerInteractionState::Answered)
    throw Error(ErrorCode::Validation, "Only questions accept an answer");
  if (interaction.type == WorkerInteractionType::Question &&
      (state == WorkerInteractionState::Approved || state == WorkerInteractionState::Denied))
    throw Error(ErrorCode::Validation, "Questions require an answer or cancellation");
  interaction.state = state;
  interaction.response = Json{{"decision", state}, {"payload", payload}, {"reason", reason}};
  interaction.reason = reason;
  interaction.actor = actor;
  interaction.decided_at = timestamp();
  storage_.commit(
      {{RecordKind::WorkerInteraction, interaction.id, interaction.run_id, Json(interaction)}});
  interaction_changed_.notify_all();
}

void WorkerManager::apply_event(const Event &event) {
  if (stopped_ || event.type.rfind("worker.job.", 0) != 0 || !event.payload.is_object())
    return;
  std::lock_guard state_lock(state_mutex_);
  const auto job_id = event.payload.value("job_id", std::string{});
  if (!bounded_identifier(job_id, max_job_id_bytes))
    return;
  WorkerJob value;
  try {
    value = job(job_id);
    const auto metadata = registry_.get(value.worker_id)->metadata();
    if (event.source_id != metadata.event_source_id)
      return;
    const auto external = event.payload.value("external_job_id", std::string{});
    if (!external.empty()) {
      if (!bounded_identifier(external, max_external_id_bytes))
        return;
      if (!value.external_job_id.empty() && external != value.external_job_id)
        return;
      if (value.external_job_id.empty())
        value.external_job_id = external;
    }
    if (worker_job_terminal(value.state))
      return;
    if (event.type == "worker.job.progress") {
      const auto progress = event.payload.value("progress", Json::object());
      if (progress.dump().size() <= max_metadata_bytes)
        value.result_metadata["progress"] = progress;
      if (event.payload.contains("usage"))
        merge_usage(value.usage, event.payload.at("usage").get<WorkerUsage>());
      if (const auto violation = budget_violation(value); !violation.empty()) {
        value.state = WorkerJobState::Failed;
        value.failure_kind = WorkerFailureKind::Budget;
        value.error = violation;
        value.completed_at = timestamp();
      }
      persist(value);
      return;
    }
    const auto next = event_state(event.type);
    if (next == WorkerJobState::Unknown ||
        (next != value.state && !valid_worker_job_transition(value.state, next)))
      return;
    value.state = next;
    if (next == WorkerJobState::Running && value.started_at.empty())
      value.started_at = timestamp();
    if (next == WorkerJobState::Completed) {
      value.result = event.payload.value("result", event.payload.value("output", Json::object()));
      value.result_metadata = event.payload.value("metadata", Json::object());
      if (event.payload.contains("continuation") && !event.payload.at("continuation").is_null()) {
        const auto &continuation = event.payload.at("continuation");
        if (!continuation.is_object())
          throw Error(ErrorCode::Validation, "Worker event continuation is invalid");
        apply_continuation(
            value, OpaqueProviderContinuation{continuation.value("provider_id", std::string{}),
                                              continuation.value("provider_version", std::string{}),
                                              continuation.value("state", std::string{})});
      }
      value.artifacts = event.payload.value("artifacts", std::vector<Json>{});
      if (event.payload.contains("usage"))
        merge_usage(value.usage, event.payload.at("usage").get<WorkerUsage>());
    } else if (next == WorkerJobState::Failed) {
      value.error = bounded_error(event.payload.value("error", std::string{"Worker failed"}));
      value.failure_kind = WorkerFailureKind::Job;
    } else if (next == WorkerJobState::Cancelled) {
      value.cancellation_acknowledged = true;
    }
    if (const auto violation = budget_violation(value); !violation.empty()) {
      value.state = WorkerJobState::Failed;
      value.failure_kind = WorkerFailureKind::Budget;
      value.error = violation;
    }
    if (worker_job_terminal(value.state))
      value.completed_at = timestamp();
    persist(value);
  } catch (const Error &) {
    log_diagnostic("worker.event_rejected", {{"event_id", event.id}, {"job_id", job_id}});
  } catch (...) {
    log_diagnostic("worker.event_processing_failed", {{"event_id", event.id}});
  }
}

void WorkerManager::receive(const Event &event) {
  try {
    apply_event(event);
  } catch (...) {
    log_diagnostic("worker.event_callback_failed", {{"event_id", event.id}});
  }
}

void WorkerManager::stop() noexcept {
  stopped_ = true;
}
} // namespace laso
