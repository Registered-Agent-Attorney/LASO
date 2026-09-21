#include <algorithm>
#include <laso/pipeline/parser.hpp>
#include <laso/runtime/runtime.hpp>
#include <limits>

namespace laso {
namespace {
std::vector<Json> list_all(const Storage &storage, RecordKind kind, const std::string &run_id) {
  constexpr std::size_t page_size = 10000;
  std::vector<Json> records;
  for (std::size_t offset = 0;;) {
    auto page = storage.list(kind, run_id, page_size, offset);
    records.insert(records.end(), page.begin(), page.end());
    if (page.size() < page_size)
      return records;
    if (offset > std::numeric_limits<std::size_t>::max() - page_size)
      throw Error(ErrorCode::Storage, "Record history is too large to inspect");
    offset += page_size;
  }
}
} // namespace
Runtime::Runtime(asio::io_context &io, Config config, RuntimeDependencies dependencies)
    : io_(io), config_(std::move(config)), deps_(dependencies), nodes_(config_.max_nodes),
      models_(config_.max_models), tools_(config_.max_tools) {}
Runtime::~Runtime() = default; // Owner must drain the executor before destruction.
void Runtime::checkpoint(Run &r, const std::string &type, std::vector<Record> records) {
  std::lock_guard lock(mutex_);
  try {
    const auto stored = deps_.storage.get(RecordKind::Run, r.id).get<Run>();
    r.cancellation_requested = r.cancellation_requested || stored.cancellation_requested;
  } catch (const Error &error) {
    if (error.code != ErrorCode::NotFound)
      throw;
  }
  r.updated_at = timestamp();
  Event event;
  event.run_id = r.id;
  event.pipeline_id = r.pipeline_id;
  event.node_id = r.active_node;
  event.type = r.state == RunState::Cancelled && type == "run.completed" ? "run.cancelled" : type;
  event.metadata["state"] = r.state;
  event.metadata["provider"] = r.provider;
  event.metadata["model"] = r.model;
  event.metadata["tool"] = r.tool;
  event.metadata["plugin"] = r.plugin;
  event.metadata["pipeline_version"] = r.pipeline_version;
  event.metadata["parent_run_id"] = r.parent_id;
  event.metadata["parent_node_id"] = r.parent_node_id;
  event.metadata["child_run_id"] = r.child_id;
  event.metadata["child_pipeline_id"] = r.child_pipeline_id;
  event.metadata["child_pipeline_version"] = r.child_pipeline_version;
  event.metadata["initiation_type"] = r.initiation_type;
  event.metadata["schedule_id"] = r.schedule_id;
  event.metadata["schedule_occurrence_id"] = r.schedule_occurrence_id;
  event.metadata["due_at"] = r.due_at;
  event.metadata["trigger_id"] = r.trigger_id;
  event.metadata["event_id"] = r.event_id;
  event.metadata["root_event_id"] = r.root_event_id;
  event.metadata["trigger_depth"] = r.trigger_depth;
  event.causation_id = r.id;
  event.root_event_id = r.root_event_id;
  event.trigger_depth = r.trigger_depth;
  for (const auto &record : records)
    if (record.kind == RecordKind::Attempt)
      event.node_id = record.value.at("node_id").get<std::string>();
  records.push_back({RecordKind::Run, r.id, r.id, Json(r)});
  records.push_back({RecordKind::Event, event.id, r.id, Json(event)});
  deps_.storage.commit(records);
  deps_.events.publish(event);
  log_event(event);
}
void Runtime::transition(Run &r, RunState state, const std::string &event,
                         std::vector<Record> records) {
  if (!valid_transition(r.state, state))
    throw Error(ErrorCode::Execution, "Invalid run state transition");
  r.state = state;
  checkpoint(r, event, std::move(records));
}
std::string Runtime::run(const PipelineDefinition &p, Json input, std::string actor,
                         std::string parent_id, std::string parent_node_id,
                         unsigned subpipeline_depth, std::string parent_message_id, Json origin) {
  std::lock_guard lock(mutex_);
  if (stopping_ || active_.size() >= config_.max_runs)
    throw Error(ErrorCode::Capacity, "Concurrent run limit reached");
  if (input.dump().size() > max_document_bytes)
    throw Error(ErrorCode::Validation, "Run input exceeds 1 MiB");
  // Do not execute a caller-modified graph that differs from the durable source.
  auto names = deps_.nodes.names();
  auto checked = parse_pipeline(p.source, {names.begin(), names.end()});
  checked.resolved_subpipelines = p.resolved_subpipelines;
  if (subpipeline_depth > config_.max_subpipeline_depth)
    throw Error(ErrorCode::Execution, "Subpipeline nesting limit exceeded");
  deps_.schemas.validate(checked.input_schema, input, checked.name, "pipeline_input");
  Run r;
  r.pipeline_id = checked.name;
  r.pipeline_version = checked.version;
  r.definition = checked.source;
  r.actor = std::move(actor);
  r.parent_id = std::move(parent_id);
  r.parent_node_id = std::move(parent_node_id);
  r.parent_message_id = std::move(parent_message_id);
  r.subpipeline_depth = subpipeline_depth;
  r.initiation_type = origin.value("initiation_type", std::string{"manual"});
  r.schedule_id = origin.value("schedule_id", std::string{});
  r.schedule_occurrence_id = origin.value("schedule_occurrence_id", std::string{});
  r.due_at = origin.value("due_at", std::string{});
  r.trigger_id = origin.value("trigger_id", std::string{});
  r.event_id = origin.value("event_id", std::string{});
  r.root_event_id = origin.value("root_event_id", std::string{});
  r.trigger_depth = origin.value("trigger_depth", 0U);
  r.resolved_subpipelines = checked.resolved_subpipelines;
  r.message.run_id = r.id;
  r.message.pipeline_id = r.pipeline_id;
  r.message.node_id = "input";
  r.message.payload = std::move(input);
  if (!r.parent_message_id.empty())
    r.message.provenance.push_back(
        {r.parent_node_id, "", "", "", "", r.parent_message_id, "", timestamp(), ""});
  checkpoint(r, "run.created");
  auto id = r.id;
  schedule(std::move(r));
  return id;
}
void Runtime::schedule(Run r) {
  if (active_.contains(r.id))
    throw Error(ErrorCode::Conflict, "Run already executing");
  if (stopping_ || active_.size() >= config_.max_runs)
    throw Error(ErrorCode::Capacity, "Concurrent run limit reached");
  auto id = r.id;
  auto [it, inserted] = active_.emplace(id, std::stop_source{});
  (void)inserted;
  auto stop = it->second.get_token();
  asio::co_spawn(io_, execute(std::move(r), stop), [this, id](const std::exception_ptr &error) {
    std::lock_guard lock(mutex_);
    active_.erase(id);
    try {
      auto ended = deps_.storage.get(RecordKind::Run, id).get<Run>();
      if (error && !terminal(ended.state) && ended.state != RunState::WaitingApproval &&
          ended.state != RunState::Paused) {
        ended.error = "Unhandled executor failure";
        transition(ended, RunState::Failed, "run.failed");
      }
      finish_parent(ended);
      if (ended.state == RunState::Paused && !ended.child_id.empty() && !stopping_) {
        auto child = deps_.storage.get(RecordKind::Run, ended.child_id).get<Run>();
        if (terminal(child.state))
          resume(ended.id);
      }
    } catch (...) {
      log_diagnostic("runtime.persistence_or_resume_failure", {{"run_id", id}});
    }
  });
}
void Runtime::resume(const std::string &id) {
  std::lock_guard lock(mutex_);
  auto r = deps_.storage.get(RecordKind::Run, id).get<Run>();
  if (active_.contains(id))
    throw Error(ErrorCode::Conflict, "Run is still leaving its checkpoint; retry shortly");
  if (r.state != RunState::WaitingApproval && r.state != RunState::Paused &&
      r.state != RunState::Queued)
    throw Error(ErrorCode::Conflict, "Run cannot resume from this state");
  if (r.state == RunState::WaitingApproval && !approved(r))
    throw Error(ErrorCode::Policy, "Approval is pending");
  if (r.cancellation_requested)
    throw Error(ErrorCode::Conflict, "Run cancellation has already been requested");
  if (stopping_ || active_.size() >= config_.max_runs)
    throw Error(ErrorCode::Capacity, "Concurrent run limit reached");
  if (r.state != RunState::Queued)
    transition(r, RunState::Queued, "run.resumed");
  schedule(std::move(r));
}
void Runtime::cancel(const std::string &id) {
  std::set<std::string> visited;
  std::vector<std::string> worker_jobs;
  {
    std::lock_guard lock(mutex_);
    cancel_locked(id, visited, worker_jobs);
  }
  // Adapter callbacks may emit events synchronously.  Never invoke native
  // worker code while the runtime mutex is held, or an event-triggered
  // callback could re-enter Runtime and deadlock.
  if (deps_.workers)
    for (const auto &worker_job_id : worker_jobs) {
      try {
        deps_.workers->cancel(worker_job_id, WorkerJobState::Cancelled,
                              "LASO run cancellation requested");
      } catch (const Error &) {
        log_diagnostic("worker.cancellation_persist_failed",
                       {{"worker_job_id", worker_job_id}, {"run_id", id}});
      }
    }
}
void Runtime::cancel_locked(const std::string &id, std::set<std::string> &visited,
                            std::vector<std::string> &worker_jobs) {
  if (!visited.insert(id).second)
    throw Error(ErrorCode::Conflict, "Run cancellation cycle detected");
  auto r = deps_.storage.get(RecordKind::Run, id).get<Run>();
  if (terminal(r.state))
    throw Error(ErrorCode::Conflict, "Run is already terminal");
  r.cancellation_requested = true;
  std::vector<Record> cancellation_records;
  for (const auto &record : list_all(deps_.storage, RecordKind::Approval, id)) {
    auto approval = record.get<Approval>();
    if (approval.decision == "pending") {
      approval.decision = "cancelled";
      approval.decided_at = timestamp();
      cancellation_records.push_back({RecordKind::Approval, approval.id, id, Json(approval)});
    }
  }
  checkpoint(r, "run.cancellation_requested", std::move(cancellation_records));
  std::set<std::string> children;
  if (!r.child_id.empty())
    children.insert(r.child_id);
  // A parent can have several active branch children.  The relationship is
  // persisted on each child, so cancellation does not depend on a single
  // mutable pointer in the parent checkpoint.
  for (const auto &record : list_all(deps_.storage, RecordKind::Run, "")) {
    const auto child = record.get<Run>();
    if (child.parent_id == id)
      children.insert(child.id);
  }
  for (const auto &child_id : children) {
    auto child = deps_.storage.get(RecordKind::Run, child_id).get<Run>();
    if (!terminal(child.state))
      cancel_locked(child.id, visited, worker_jobs);
  }
  if (deps_.workers) {
    for (const auto &record : deps_.storage.list(RecordKind::WorkerJob, id, 10000, 0)) {
      const auto worker_job = record.get<WorkerJob>();
      if (!worker_job_terminal(worker_job.state))
        worker_jobs.push_back(worker_job.id);
    }
  }
  auto found = active_.find(id);
  if (found != active_.end()) {
    found->second.request_stop();
    if (r.state == RunState::WaitingApproval || r.state == RunState::Paused)
      transition(r, RunState::Cancelled, "run.cancelled");
  } else
    transition(r, RunState::Cancelled, "run.cancelled");
}
void Runtime::shutdown() {
  std::lock_guard lock(mutex_);
  stopping_ = true;
  for (auto &[id, source] : active_) {
    (void)id;
    source.request_stop();
  }
}
bool Runtime::idle() const {
  std::lock_guard lock(mutex_);
  return active_.empty();
}
bool Runtime::approved(const Run &r) const {
  for (const auto &item : list_all(deps_.storage, RecordKind::Approval, r.id)) {
    auto a = item.get<Approval>();
    auto found = r.node_visits.find(r.active_node);
    auto visit = found == r.node_visits.end() ? 0 : found->second;
    if (a.node_id == r.active_node && a.visit == visit && a.decision == "approved")
      return true;
  }
  return false;
}
void Runtime::wait_approval(Run &r, const NodeDefinition &node, const std::string &reason) {
  Approval a;
  a.run_id = r.id;
  a.node_id = node.id;
  a.reason = reason;
  a.action = node.type + ":" + node.binding;
  a.visit = r.node_visits[node.id];
  NodeExecution attempt;
  attempt.run_id = r.id;
  attempt.node_id = node.id;
  attempt.state = NodeState::WaitingApproval;
  transition(r, RunState::WaitingApproval, "approval.requested",
             {{RecordKind::Approval, a.id, r.id, Json(a)},
              {RecordKind::Attempt, attempt.id, r.id, Json(attempt)}});
}
void Runtime::decide(const std::string &id, bool approve, const std::string &actor,
                     const std::string &comment) {
  std::lock_guard lock(mutex_);
  if (actor.size() > 128 || comment.size() > 2048)
    throw Error(ErrorCode::Validation, "Approval annotation exceeds limit");
  auto a = deps_.storage.get(RecordKind::Approval, id).get<Approval>();
  auto r = deps_.storage.get(RecordKind::Run, a.run_id).get<Run>();
  if (a.decision != "pending" || r.state != RunState::WaitingApproval || r.active_node != a.node_id)
    throw Error(ErrorCode::Conflict, "Approval is no longer pending");
  if (active_.contains(r.id))
    throw Error(ErrorCode::Conflict, "Approval checkpoint is settling; retry shortly");
  if (approve && (stopping_ || active_.size() >= config_.max_runs))
    throw Error(ErrorCode::Capacity, "Concurrent run limit reached");
  a.decision = approve ? "approved" : "rejected";
  a.decided_at = timestamp();
  a.actor = actor;
  a.comment = comment;
  std::vector<Record> records{{RecordKind::Approval, a.id, r.id, Json(a)}};
  for (const auto &item : list_all(deps_.storage, RecordKind::Attempt, r.id)) {
    auto attempt = item.get<NodeExecution>();
    if (attempt.node_id == a.node_id && attempt.state == NodeState::WaitingApproval) {
      attempt.state = approve ? NodeState::Completed : NodeState::Failed;
      attempt.finished_at = timestamp();
      records.push_back({RecordKind::Attempt, attempt.id, r.id, Json(attempt)});
    }
  }
  if (approve) {
    // Decision and resumable queue checkpoint are one transaction.
    transition(r, RunState::Queued, "approval.approved", std::move(records));
    schedule(std::move(r));
  } else {
    r.error = "Human approval rejected";
    transition(r, RunState::Failed, "approval.rejected", std::move(records));
    finish_parent(r);
  }
}
void Runtime::finish_parent(const Run &child) {
  if (child.parent_id.empty() || !terminal(child.state) || stopping_)
    return;
  auto parent = deps_.storage.get(RecordKind::Run, child.parent_id).get<Run>();
  if (parent.state == RunState::Paused && !active_.contains(parent.id))
    resume(parent.id);
}
} // namespace laso
