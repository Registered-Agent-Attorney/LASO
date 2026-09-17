#include <algorithm>
#include <cctype>
#include <cmath>
#include <laso/core/config.hpp>
#include <laso/workers/manager.hpp>
#include <limits>
#include <set>
#include <sstream>

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

std::string bounded_error(const std::string &value) {
  if (value.size() <= 512)
    return value;
  return value.substr(0, 512);
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

WorkerJob WorkerManager::job(const std::string &id) const {
  if (!bounded_identifier(id, max_job_id_bytes))
    throw Error(ErrorCode::Validation, "Invalid worker job id");
  return storage_.get(RecordKind::WorkerJob, id).get<WorkerJob>();
}

std::vector<Json> WorkerManager::jobs(const std::string &run_id, std::size_t limit,
                                      std::size_t offset) const {
  return storage_.list(RecordKind::WorkerJob, run_id, limit, offset);
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
  if (metric_exceeds(value.usage.queue_duration_ms) || metric_exceeds(value.usage.wall_duration_ms) ||
      metric_exceeds(value.usage.input_tokens) || metric_exceeds(value.usage.output_tokens) ||
      metric_exceeds(value.usage.total_tokens) || metric_exceeds(value.usage.tool_calls) ||
      metric_exceeds(value.usage.action_count))
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
        tokens && (prior_tokens > max_tokens_per_run_ ||
                   *tokens > max_tokens_per_run_ - prior_tokens))
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
  storage_.commit({{RecordKind::WorkerJob, value.id, value.run_id, Json(value)}});
}

WorkerJob WorkerManager::reconcile(WorkerJob value) {
  if (worker_job_terminal(value.state) || value.external_job_id.empty())
    return value;
  auto adapter = registry_.get(value.worker_id);
  if (!adapter->metadata().supports_recovery)
    return value;
  try {
    const auto status = adapter->status(value.external_job_id);
    if (status.state == WorkerJobState::Unknown)
      return value;
    if (!valid_worker_job_transition(value.state, status.state) && value.state != status.state)
      return value;
    if (status.state != value.state)
      value.state = status.state;
    if (!status.result.is_null() && status.result.dump().size() <= max_result_bytes)
      value.result = status.result;
    if (status.metadata.is_object() && status.metadata.dump().size() <= max_metadata_bytes)
      value.result_metadata = status.metadata;
    merge_usage(value.usage, status.usage);
    if (!status.artifacts.empty() && status.artifacts.size() <= max_artifacts_)
      value.artifacts = status.artifacts;
    if (!status.error.empty())
      value.error = bounded_error(status.error);
    if (const auto violation = budget_violation(value); !violation.empty()) {
      value.state = WorkerJobState::Failed;
      value.failure_kind = WorkerFailureKind::Budget;
      value.error = violation;
    } else if (value.state == WorkerJobState::Failed && value.failure_kind == WorkerFailureKind::None) {
      value.failure_kind = WorkerFailureKind::Job;
    }
    if (worker_job_terminal(value.state) && value.completed_at.empty())
      value.completed_at = timestamp();
    persist(value);
  } catch (const Error &) { // NOLINT(bugprone-empty-catch): recovery is best effort.
    // Recovery is deliberately conservative. An adapter failure does not
    // invent a terminal result or submit a second external task.
  } catch (...) { // NOLINT(bugprone-empty-catch): contain native adapter failures.
    // Native adapter exceptions are contained by the loader; this is an
    // additional safety boundary around recovery.
  }
  return value;
}

WorkerJob WorkerManager::submit(const WorkerRequest &request) {
  std::lock_guard submit_lock(submit_mutex_);
  if (stopped_)
    throw Error(ErrorCode::Conflict, "Worker manager is stopped");
  if (request.idempotency_key.empty() || request.idempotency_key.size() > 512 ||
      request.task_type.size() > 128 || request.capability.size() > 128 ||
      request.instructions.size() > max_result_bytes ||
      request.input.dump().size() > max_result_bytes ||
      request.metadata.dump().size() > max_metadata_bytes ||
      request.artifact_ids.size() > max_artifacts_)
    throw Error(ErrorCode::Validation, "Worker request exceeds limit");
  for (const auto &artifact : request.artifact_ids)
    if (!bounded_identifier(artifact, 128))
      throw Error(ErrorCode::Validation, "Invalid worker artifact reference");

  auto worker_id = resolve_worker(request.worker_id, request.capability);
  auto adapter = registry_.get(worker_id);
  const auto metadata = adapter->metadata();
  if (!metadata.enabled || metadata.status == "disabled" || metadata.status == "failed" ||
      metadata.status == "unavailable")
    throw Error(ErrorCode::Capacity, "Worker is unavailable");

  const auto durable_id = durable_job_id(request.idempotency_key);
  auto existing_job = [&]() -> std::optional<WorkerJob> {
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
  if (existing.has_value()) {
    if (!worker_id.empty() && existing->worker_id != worker_id)
      throw Error(ErrorCode::Conflict, "Worker idempotency key belongs to another worker");
    if (worker_job_terminal(existing->state))
      return *existing;
    if (!existing->external_job_id.empty())
      return reconcile(std::move(*existing));
    if (!metadata.supports_recovery) {
      existing->state = WorkerJobState::Unknown;
      existing->error = "External submission outcome is unknown; adapter recovery is unavailable";
      persist(*existing);
      return *existing;
    }
    auto retry_request = request;
    retry_request.job_id = existing->id;
    auto submission = adapter->submit(retry_request);
    existing->external_job_id = submission.external_job_id;
    existing->state = submission.state;
    existing->result_metadata = submission.metadata;
    merge_usage(existing->usage, submission.usage);
    if (existing->external_job_id.empty() ||
        existing->external_job_id.size() > max_external_id_bytes)
      throw Error(ErrorCode::Plugin, "Worker returned an invalid external job id");
    if (worker_job_terminal(existing->state))
      existing->completed_at = timestamp();
    if (const auto violation = budget_violation(*existing); !violation.empty()) {
      existing->state = WorkerJobState::Failed;
      existing->failure_kind = WorkerFailureKind::Budget;
      existing->error = violation;
      existing->completed_at = timestamp();
    } else if (existing->state == WorkerJobState::Failed) {
      existing->failure_kind = WorkerFailureKind::Job;
    }
    persist(*existing);
    return *existing;
  }

  std::size_t active = 0, worker_active = 0;
  for (const auto &record : storage_.list(RecordKind::WorkerJob, "", 10000, 0)) {
    const auto stored = record.get<WorkerJob>();
    if (worker_job_terminal(stored.state))
      continue;
    ++active;
    if (stored.worker_id == worker_id)
      ++worker_active;
  }
  if (active >= max_active_)
    throw Error(ErrorCode::Capacity, "Worker job capacity is exhausted");
  if (worker_active >= max_per_worker_)
    throw Error(ErrorCode::Capacity, "Worker-specific job capacity is exhausted");

  WorkerJob created;
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
  if (request.metadata.is_object() && request.metadata.contains("classification"))
    created.request_metadata["classification"] = request.metadata.at("classification");
  if (!storage_.claim({RecordKind::WorkerJob, created.id, created.run_id, Json(created)})) {
    existing = existing_job();
    if (!existing.has_value())
      throw Error(ErrorCode::Storage, "Worker job claim disappeared");
    return *existing;
  }
  created.state = WorkerJobState::Submitting;
  persist(created);

  try {
    auto outbound = request;
    outbound.job_id = created.id;
    const auto submission = adapter->submit(outbound);
    if (submission.external_job_id.empty() ||
        submission.external_job_id.size() > max_external_id_bytes)
      throw Error(ErrorCode::Plugin, "Worker returned an invalid external job id");
    const auto observed = job(created.id);
    if (worker_job_terminal(observed.state))
      return observed;
    created.external_job_id = submission.external_job_id;
    created.state = submission.state;
    created.result_metadata = submission.metadata;
    merge_usage(created.usage, submission.usage);
    if (worker_job_terminal(created.state))
      created.completed_at = timestamp();
    if (const auto violation = budget_violation(created); !violation.empty()) {
      created.state = WorkerJobState::Failed;
      created.failure_kind = WorkerFailureKind::Budget;
      created.error = violation;
      created.completed_at = timestamp();
    } else if (created.state == WorkerJobState::Failed) {
      created.failure_kind = WorkerFailureKind::Job;
    }
    persist(created);
  } catch (const WorkerTransportError &error) {
    created.state = WorkerJobState::Failed;
    created.failure_kind = WorkerFailureKind::Transport;
    created.error = bounded_error(error.what());
    log_diagnostic("worker.transport_failed", { {"worker_job_id", created.id},
                                                  {"worker_id", created.worker_id} });
    persist(created);
  } catch (const Error &error) {
    created.state = WorkerJobState::Failed;
    created.failure_kind = WorkerFailureKind::Job;
    created.error =
        bounded_error(error.code == ErrorCode::Plugin ? error.what() : "Worker submission failed");
    log_diagnostic("worker.submission_failed", {{"worker_job_id", created.id},
                                                {"worker_id", created.worker_id},
                                                {"reason", created.error}});
    persist(created);
  } catch (...) {
    created.state = WorkerJobState::Failed;
    created.error = "Worker submission failed";
    log_diagnostic("worker.submission_failed",
                   {{"worker_job_id", created.id}, {"worker_id", created.worker_id}});
    persist(created);
  }
  return created;
}

void WorkerManager::cancel(const std::string &id, WorkerJobState requested_state,
                           const std::string &reason) {
  auto value = job(id);
  if (worker_job_terminal(value.state))
    return;
  value.cancellation_requested = true;
  value.cancellation_error = bounded_error(reason);
  if (value.external_job_id.empty()) {
    if (requested_state == WorkerJobState::TimedOut) {
      value.state = WorkerJobState::TimedOut;
      value.completed_at = timestamp();
    }
    persist(value);
    return;
  }
  try {
    const auto acknowledged = registry_.get(value.worker_id)->cancel(value.external_job_id);
    if (requested_state == WorkerJobState::TimedOut) {
      value.state = WorkerJobState::TimedOut;
      value.completed_at = timestamp();
    } else if (acknowledged) {
      value.cancellation_acknowledged = true;
      value.state = WorkerJobState::Cancelled;
      value.completed_at = timestamp();
    } else {
      value.cancellation_error = "Worker did not acknowledge cancellation";
    }
  } catch (...) {
    value.cancellation_error = "Worker cancellation failed";
    if (requested_state == WorkerJobState::TimedOut) {
      value.state = WorkerJobState::TimedOut;
      value.completed_at = timestamp();
    }
  }
  persist(value);
}

void WorkerManager::apply_event(const Event &event) {
  if (stopped_ || event.type.rfind("worker.job.", 0) != 0 || !event.payload.is_object())
    return;
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
