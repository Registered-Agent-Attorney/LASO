#include <algorithm>
#include <laso/pipeline/parser.hpp>
#include <laso/runtime/runtime.hpp>
#include <limits>

namespace laso {
namespace {
constexpr std::size_t max_message_metadata_bytes = std::size_t{64} * 1024;

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
      models_(config_.max_models), tools_(config_.max_tools),
      claim_timer_(std::make_shared<asio::steady_timer>(io)),
      lease_timer_(std::make_shared<asio::steady_timer>(io)) {}
Runtime::~Runtime() = default; // Owner must drain the executor before destruction.
void Runtime::start_distributed() {
  if (!deps_.coordination || distributed_started_)
    return;
  deps_.coordination->register_instance(version, "runtime,run-claims,fencing");
  distributed_started_ = true;
  asio::co_spawn(io_, claim_loop(), asio::detached);
  asio::co_spawn(io_, lease_loop(), asio::detached);
}
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
  try {
    if (deps_.coordination && !r.owner_instance_id.empty())
      deps_.storage.commit_owned(records, "run:" + r.id, r.owner_instance_id, r.fencing_token);
    else
      deps_.storage.commit(records);
  } catch (const Error &error) {
    if (deps_.coordination && error.code == ErrorCode::Conflict) {
      auto active = active_.find(r.id);
      if (active != active_.end()) {
        active->second.ownership_lost = true;
        active->second.stop.request_stop();
      }
    }
    throw;
  }
  deps_.events.publish(event);
  log_event(event);
}
void Runtime::commit_node_owned(const std::vector<Record> &records, const NodeWork &work,
                                const LeaseRecord &lease) {
  deps_.storage.commit_owned(records, "node:" + work.id, lease.owner_instance,
                             lease.fencing_token);
}
bool Runtime::distributed_parallel_ready(const Run &r) const {
  if (r.pending_parallel_group.empty())
    return false;
  const auto works = deps_.storage.list(RecordKind::NodeWork, r.id, 10000, 0);
  bool found = false;
  for (const auto &value : works) {
    const auto work = value.get<NodeWork>();
    if (work.group_id != r.pending_parallel_group)
      continue;
    found = true;
    if (work.state == NodeWorkState::Queued || work.state == NodeWorkState::Running)
      return false;
  }
  return found;
}
void Runtime::reconcile_distributed_parallel(Run &r, const PipelineDefinition &) {
  if (r.pending_parallel_group.empty())
    return;
  std::vector<NodeWork> works;
  for (const auto &value : deps_.storage.list(RecordKind::NodeWork, r.id, 10000, 0)) {
    auto work = value.get<NodeWork>();
    if (work.group_id == r.pending_parallel_group)
      works.push_back(std::move(work));
  }
  if (works.empty())
    throw Error(ErrorCode::Execution, "Distributed parallel work is missing");
  std::sort(works.begin(), works.end(),
            [](const NodeWork &left, const NodeWork &right) { return left.index < right.index; });
  for (const auto &work : works)
    if (work.state != NodeWorkState::Completed || !work.result)
      throw Error(ErrorCode::Execution,
                  work.error.empty() ? "Distributed parallel branch failed" : work.error);
  r.message.payload = Json::array();
  r.steps = 0;
  for (const auto &work : works) {
    r.message.payload.push_back(work.result->payload);
    r.message.provenance.insert(r.message.provenance.end(), work.result->provenance.begin(),
                                work.result->provenance.end());
    r.steps = std::max(r.steps, work.steps);
  }
  r.frames.clear();
  r.active_node = r.pending_parallel_join;
  r.prepared_join = r.active_node;
  r.pending_parallel_group.clear();
  r.pending_parallel_join.clear();
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
                         unsigned subpipeline_depth, std::string parent_message_id, Json origin,
                         Json message_metadata) {
  std::lock_guard lock(mutex_);
  if (stopping_ || (!deps_.coordination && active_.size() >= config_.max_runs))
    throw Error(ErrorCode::Capacity, "Concurrent run limit reached");
  if (deps_.coordination) {
    std::size_t pending = 0;
    for (std::size_t offset = 0;;) {
      const auto page = deps_.storage.list(RecordKind::Run, "", 1000, offset);
      for (const auto &record : page)
        if (record.get<Run>().state == RunState::Queued && ++pending >= config_.max_pending_runs)
          throw Error(ErrorCode::Capacity, "Distributed run queue is full");
      if (page.size() < 1000)
        break;
      if (offset > std::numeric_limits<std::size_t>::max() - 1000)
        throw Error(ErrorCode::Storage, "Distributed run queue is too large to inspect");
      offset += 1000;
    }
  }
  if (input.dump().size() > max_document_bytes)
    throw Error(ErrorCode::Validation, "Run input exceeds 1 MiB");
  if (!message_metadata.is_object() || message_metadata.dump().size() > max_message_metadata_bytes)
    throw Error(ErrorCode::Validation, "Run metadata is invalid or exceeds 64 KiB");
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
  r.message.metadata = std::move(message_metadata);
  if (!r.parent_message_id.empty())
    r.message.provenance.push_back(
        {r.parent_node_id, "", "", "", "", r.parent_message_id, "", timestamp(), ""});
  checkpoint(r, "run.created");
  auto id = r.id;
  if (!deps_.coordination)
    schedule(std::move(r));
  return id;
}
void Runtime::schedule(Run r, std::optional<LeaseRecord> lease) {
  if (active_.contains(r.id))
    throw Error(ErrorCode::Conflict, "Run already executing");
  if (stopping_ || active_.size() >= config_.max_runs)
    throw Error(ErrorCode::Capacity, "Concurrent run limit reached");
  if (deps_.coordination && !lease)
    throw Error(ErrorCode::Conflict, "Distributed run is missing an ownership lease");
  auto id = r.id;
  ActiveRun active;
  active.lease = std::move(lease);
  auto [it, inserted] = active_.emplace(id, std::move(active));
  (void)inserted;
  auto stop = it->second.stop.get_token();
  asio::co_spawn(io_, execute(std::move(r), stop), [this, id](const std::exception_ptr &error) {
    std::lock_guard lock(mutex_);
    bool ownership_lost = false;
    std::optional<LeaseRecord> lease;
    if (const auto active = active_.find(id); active != active_.end()) {
      ownership_lost = active->second.ownership_lost;
      lease = active->second.lease;
    }
    active_.erase(id);
    try {
      auto ended = deps_.storage.get(RecordKind::Run, id).get<Run>();
      if (error && !ownership_lost && !terminal(ended.state) &&
          ended.state != RunState::WaitingApproval && ended.state != RunState::Paused) {
        ended.error = "Unhandled executor failure";
        transition(ended, RunState::Failed, "run.failed");
      }
      finish_parent(ended);
      if (ended.state == RunState::Paused && !ended.child_id.empty() && !stopping_) {
        auto child = deps_.storage.get(RecordKind::Run, ended.child_id).get<Run>();
        if (terminal(child.state))
          resume(ended.id);
      }
      if (lease && deps_.coordination)
        deps_.coordination->release(*lease);
    } catch (...) {
      log_diagnostic("runtime.persistence_or_resume_failure", {{"run_id", id}});
    }
  });
}
Task<void> Runtime::claim_loop() {
  for (;;) {
    claim_timer_->expires_after(Milliseconds{100});
    boost::system::error_code wait_error;
    co_await claim_timer_->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
    if (wait_error)
      co_return;
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        co_return;
      if (active_.size() >= config_.max_runs)
        continue;
    }
    std::vector<Json> candidates;
    try {
      candidates = deps_.storage.list(RecordKind::Run, "", config_.claim_batch_size, 0);
    } catch (const Error &) {
      continue;
    }
    for (const auto &value : candidates) {
      Run run;
      try {
        run = value.get<Run>();
      } catch (const Json::exception &) {
        continue;
      }
      if (terminal(run.state) || run.state == RunState::WaitingApproval)
        continue;
      if (run.state == RunState::Paused && !run.pending_parallel_group.empty()) {
        try {
          if (!distributed_parallel_ready(run))
            continue;
        } catch (const Error &) {
          continue;
        }
      } else if (run.state == RunState::Paused && !run.child_id.empty()) {
        try {
          const auto child = deps_.storage.get(RecordKind::Run, run.child_id).get<Run>();
          if (!terminal(child.state))
            continue;
        } catch (const Error &) {
          continue;
        }
      }
      {
        std::lock_guard lock(mutex_);
        if (stopping_ || active_.size() >= config_.max_runs || active_.contains(run.id))
          break;
      }
      std::optional<LeaseRecord> lease;
      try {
        lease = deps_.coordination->acquire("run:" + run.id, config_.coordination_lease_ttl_ms);
      } catch (const Error &) {
        continue;
      }
      if (!lease)
        continue;
      try {
        run = deps_.storage.get(RecordKind::Run, run.id).get<Run>();
        if (terminal(run.state) || run.state == RunState::WaitingApproval) {
          deps_.coordination->release(*lease);
          continue;
        }
        // A takeover replays from the durable node checkpoint.  This avoids
        // attempting to continue an in-flight coroutine that died with the
        // previous process.
        run.state = RunState::Queued;
        run.owner_instance_id = lease->owner_instance;
        run.fencing_token = lease->fencing_token;
        run.claimed_at = lease->acquired_at;
        run.last_renewed_at = lease->heartbeat_at;
        run.lease_expires_at = lease->expires_at;
        deps_.storage.commit_owned({{RecordKind::Run, run.id, run.id, Json(run)}},
                                   lease->resource_key, lease->owner_instance,
                                   lease->fencing_token);
        std::lock_guard lock(mutex_);
        if (stopping_ || active_.size() >= config_.max_runs) {
          deps_.coordination->release(*lease);
          continue;
        }
        schedule(std::move(run), std::move(lease));
      } catch (const Error &) {
        if (lease) {
          try {
            deps_.coordination->release(*lease);
          } catch (const Error &) {
          }
        }
      }
    }
    std::vector<Json> node_candidates;
    try {
      node_candidates = deps_.storage.list(RecordKind::NodeWork, "", config_.claim_batch_size, 0);
    } catch (const Error &) {
      continue;
    }
    for (const auto &value : node_candidates) {
      NodeWork work;
      try {
        work = value.get<NodeWork>();
        const auto run = deps_.storage.get(RecordKind::Run, work.run_id).get<Run>();
        if (terminal(run.state) || run.cancellation_requested ||
            run.pending_parallel_group != work.group_id)
          continue;
      } catch (const Error &) {
        continue;
      }
      {
        std::lock_guard lock(mutex_);
        if (stopping_ || active_nodes_.size() >= config_.max_nodes ||
            active_nodes_.contains(work.id))
          break;
      }
      std::optional<LeaseRecord> work_lease;
      std::optional<LeaseRecord> global_slot;
      std::optional<LeaseRecord> run_slot;
      try {
        work_lease = deps_.coordination->acquire("node:" + work.id,
                                                config_.coordination_lease_ttl_ms);
        if (!work_lease)
          continue;
        for (unsigned slot = 0; slot < config_.max_nodes && !global_slot; ++slot)
          global_slot = deps_.coordination->acquire(
              "node-slot:" + std::to_string(slot), config_.coordination_lease_ttl_ms);
        if (!global_slot) {
          deps_.coordination->release(*work_lease);
          continue;
        }
        for (unsigned slot = 0; slot < config_.max_nodes_per_run && !run_slot; ++slot)
          run_slot = deps_.coordination->acquire(
              "run-node-slot:" + work.run_id + ":" + std::to_string(slot),
              config_.coordination_lease_ttl_ms);
        if (!run_slot) {
          deps_.coordination->release(*global_slot);
          deps_.coordination->release(*work_lease);
          continue;
        }
        work = deps_.storage.get(RecordKind::NodeWork, work.id).get<NodeWork>();
        if (work.state == NodeWorkState::Completed || work.state == NodeWorkState::Failed ||
            work.state == NodeWorkState::Cancelled) {
          deps_.coordination->release(*run_slot);
          deps_.coordination->release(*global_slot);
          deps_.coordination->release(*work_lease);
          continue;
        }
        work.state = NodeWorkState::Running;
        ++work.attempt;
        work.attempt_id = uuid();
        work.owner_instance_id = work_lease->owner_instance;
        work.fencing_token = work_lease->fencing_token;
        work.claimed_at = work_lease->acquired_at;
        work.last_renewed_at = work_lease->heartbeat_at;
        work.lease_expires_at = work_lease->expires_at;
        work.updated_at = timestamp();
        commit_node_owned({{RecordKind::NodeWork, work.id, work.run_id, Json(work)}}, work,
                          *work_lease);
        std::lock_guard lock(mutex_);
        if (stopping_ || active_nodes_.size() >= config_.max_nodes) {
          deps_.coordination->release(*run_slot);
          deps_.coordination->release(*global_slot);
          deps_.coordination->release(*work_lease);
          continue;
        }
        ActiveNode active;
        active.work_lease = *work_lease;
        active.global_slot = *global_slot;
        active.run_slot = *run_slot;
        auto [it, inserted] = active_nodes_.emplace(work.id, std::move(active));
        if (!inserted)
          throw Error(ErrorCode::Conflict, "Node work is already executing");
        auto stop = it->second.stop.get_token();
        const auto id = work.id;
        asio::co_spawn(
            io_, execute_distributed_work(std::move(work), *work_lease, *global_slot, *run_slot,
                                          stop),
            [this, id](const std::exception_ptr &error) {
              std::lock_guard lock(mutex_);
              std::optional<LeaseRecord> work_lease;
              std::optional<LeaseRecord> global_slot;
              std::optional<LeaseRecord> run_slot;
              if (const auto active = active_nodes_.find(id); active != active_nodes_.end()) {
                work_lease = active->second.work_lease;
                global_slot = active->second.global_slot;
                run_slot = active->second.run_slot;
                active_nodes_.erase(active);
              }
              if (error)
                log_diagnostic("runtime.distributed_node_failure", {{"node_work_id", id}});
              if (run_slot)
                try {
                  deps_.coordination->release(*run_slot);
                } catch (const Error &) {
                }
              if (global_slot)
                try {
                  deps_.coordination->release(*global_slot);
                } catch (const Error &) {
                }
              if (work_lease)
                try {
                  deps_.coordination->release(*work_lease);
                } catch (const Error &) {
                }
            });
      } catch (const Error &) {
        if (run_slot)
          try {
            deps_.coordination->release(*run_slot);
          } catch (const Error &) {
          }
        if (global_slot)
          try {
            deps_.coordination->release(*global_slot);
          } catch (const Error &) {
          }
        if (work_lease)
          try {
            deps_.coordination->release(*work_lease);
          } catch (const Error &) {
          }
      }
    }
  }
}
Task<void> Runtime::lease_loop() {
  for (;;) {
    lease_timer_->expires_after(Milliseconds{config_.coordination_heartbeat_interval_ms});
    boost::system::error_code wait_error;
    co_await lease_timer_->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
    if (wait_error)
      co_return;
    struct NodeLeases {
      std::string id;
      LeaseRecord work;
      std::optional<LeaseRecord> global;
      std::optional<LeaseRecord> run;
    };
    std::vector<std::pair<std::string, LeaseRecord>> leases;
    std::vector<NodeLeases> node_leases;
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        co_return;
      for (const auto &[id, active] : active_)
        if (active.lease)
          leases.emplace_back(id, *active.lease);
      for (const auto &[id, active] : active_nodes_)
        node_leases.push_back({id, active.work_lease, active.global_slot, active.run_slot});
    }
    try {
      if (!deps_.coordination->heartbeat_instance("ACTIVE"))
        log_diagnostic("runtime.instance_heartbeat_missing", {{"instance_id", deps_.instance_id}});
    } catch (const Error &) {
      log_diagnostic("runtime.instance_heartbeat_failed", {{"instance_id", deps_.instance_id}});
    }
    for (auto &[id, lease] : leases) {
      try {
        if (!deps_.coordination->renew(lease, config_.coordination_lease_ttl_ms)) {
          std::lock_guard lock(mutex_);
          if (const auto active = active_.find(id); active != active_.end()) {
            active->second.ownership_lost = true;
            active->second.stop.request_stop();
          }
          continue;
        }
        std::lock_guard lock(mutex_);
        if (const auto active = active_.find(id); active != active_.end() && active->second.lease)
          *active->second.lease = lease;
      } catch (const Error &) {
        std::lock_guard lock(mutex_);
        if (const auto active = active_.find(id); active != active_.end()) {
          active->second.ownership_lost = true;
          active->second.stop.request_stop();
        }
      }
    }
    for (auto &node : node_leases) {
      try {
        const auto run = deps_.storage.get(RecordKind::Run,
                                           deps_.storage.get(RecordKind::NodeWork, node.id)
                                               .get<NodeWork>()
                                               .run_id)
                              .get<Run>();
        if (run.cancellation_requested || terminal(run.state)) {
          std::lock_guard lock(mutex_);
          if (const auto active = active_nodes_.find(node.id); active != active_nodes_.end())
            active->second.stop.request_stop();
        }
      } catch (const Error &) {
        std::lock_guard lock(mutex_);
        if (const auto active = active_nodes_.find(node.id); active != active_nodes_.end()) {
          active->second.ownership_lost = true;
          active->second.stop.request_stop();
        }
      }
      bool valid = true;
      for (auto *lease : {&node.work, node.global ? &*node.global : nullptr,
                          node.run ? &*node.run : nullptr}) {
        if (!lease)
          continue;
        try {
          if (!deps_.coordination->renew(*lease, config_.coordination_lease_ttl_ms))
            valid = false;
        } catch (const Error &) {
          valid = false;
        }
      }
      if (!valid) {
        std::lock_guard lock(mutex_);
        if (const auto active = active_nodes_.find(node.id); active != active_nodes_.end()) {
          active->second.ownership_lost = true;
          active->second.stop.request_stop();
        }
        continue;
      }
      std::lock_guard lock(mutex_);
      if (const auto active = active_nodes_.find(node.id); active != active_nodes_.end()) {
        active->second.work_lease = node.work;
        if (node.global)
          active->second.global_slot = node.global;
        if (node.run)
          active->second.run_slot = node.run;
      }
    }
  }
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
  if (stopping_ || (!deps_.coordination && active_.size() >= config_.max_runs))
    throw Error(ErrorCode::Capacity, "Concurrent run limit reached");
  if (r.state != RunState::Queued)
    transition(r, RunState::Queued, "run.resumed");
  if (!deps_.coordination)
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
  if (deps_.coordination) {
    deps_.storage.request_cancellation(id);
    for (const auto &record : deps_.storage.list(RecordKind::NodeWork, id, 10000, 0)) {
      auto work = record.get<NodeWork>();
      if (work.state == NodeWorkState::Queued) {
        work.state = NodeWorkState::Cancelled;
        work.error = "Parent run cancellation requested";
        work.updated_at = timestamp();
        deps_.storage.commit({{RecordKind::NodeWork, work.id, work.run_id, Json(work)}});
      } else if (work.state == NodeWorkState::Running) {
        if (const auto active = active_nodes_.find(work.id); active != active_nodes_.end())
          active->second.stop.request_stop();
      }
    }
    std::set<std::string> children;
    if (!r.child_id.empty())
      children.insert(r.child_id);
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
    if (const auto found = active_.find(id); found != active_.end()) {
      found->second.stop.request_stop();
    } else if (r.state == RunState::WaitingApproval || r.state == RunState::Paused ||
               r.state == RunState::Queued) {
      r.cancellation_requested = true;
      r.state = RunState::Cancelled;
      r.updated_at = timestamp();
      deps_.storage.commit({{RecordKind::Run, r.id, r.id, Json(r)}});
    }
    return;
  }
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
    found->second.stop.request_stop();
    if (r.state == RunState::WaitingApproval || r.state == RunState::Paused)
      transition(r, RunState::Cancelled, "run.cancelled");
  } else
    transition(r, RunState::Cancelled, "run.cancelled");
}
void Runtime::shutdown() {
  std::lock_guard lock(mutex_);
  stopping_ = true;
  claim_timer_->cancel();
  lease_timer_->cancel();
  if (deps_.coordination && distributed_started_) {
    try {
      deps_.coordination->set_instance_state("DRAINING");
    } catch (const Error &) {
    }
  }
  for (auto &[id, source] : active_) {
    (void)id;
    source.stop.request_stop();
  }
  for (auto &[id, source] : active_nodes_) {
    (void)id;
    source.stop.request_stop();
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
  if (approve && (stopping_ || (!deps_.coordination && active_.size() >= config_.max_runs)))
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
  if (deps_.coordination) {
    // Approval is a control-plane decision and may be made by an instance
    // other than the one that was executing the run.  The paused executor
    // has already persisted its checkpoint and will release its lease when
    // it returns, so the decision must not require that executor's fencing
    // proof.  Clearing the stale execution ownership also makes the queued
    // run claimable by any healthy instance.
    r.owner_instance_id.clear();
    r.lease_expires_at.clear();
    r.claimed_at.clear();
    r.last_renewed_at.clear();
    r.fencing_token = 0;
  }
  if (approve) {
    // Decision and resumable queue checkpoint are one transaction.
    transition(r, RunState::Queued, "approval.approved", std::move(records));
    if (!deps_.coordination)
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
  if (parent.state == RunState::Paused && !active_.contains(parent.id)) {
    if (deps_.coordination) {
      parent.state = RunState::Queued;
      parent.updated_at = timestamp();
      deps_.storage.commit({{RecordKind::Run, parent.id, parent.id, Json(parent)}});
    } else {
      resume(parent.id);
    }
  }
}
} // namespace laso
