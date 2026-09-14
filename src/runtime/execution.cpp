#include <algorithm>
#include <laso/pipeline/parser.hpp>
#include <laso/runtime/runtime.hpp>

namespace laso {
Task<void> Runtime::execute(Run r, std::stop_token stop) {
  auto extensions = deps_.nodes.names();
  const auto pipeline = parse_pipeline(r.definition, {extensions.begin(), extensions.end()});
  const auto deadline = std::chrono::steady_clock::now() + pipeline.timeout.timeout;
  bool attempt_recorded = false;
  try {
    {
      std::lock_guard lock(mutex_);
      transition(r, RunState::Starting, "run.starting");
      transition(r, RunState::Running, "run.started");
    }
    while (true) {
      attempt_recorded = false;
      ExecutionContext context{r.id, r.pipeline_id, r.active_node, stop, deadline};
      context.check();
      const auto &definition = pipeline.nodes.at(r.active_node);
      r.provider.clear();
      r.model.clear();
      r.tool.clear();
      r.plugin.clear();
      if (definition.type == "agent" && config_.models.contains(definition.binding)) {
        const auto &binding = config_.models.at(definition.binding);
        r.provider = binding.provider;
        r.model = binding.model;
      } else if (definition.type == "tool") {
        r.tool = definition.binding;
        r.plugin = deps_.tools.get(definition.binding)->metadata().plugin;
      }
      context.deadline =
          std::min(deadline, std::chrono::steady_clock::now() + definition.timeout.timeout);
      if (!prepare_join(r, definition))
        continue;
      if (r.steps >= pipeline.max_steps)
        throw Error(ErrorCode::Execution, "Pipeline step limit exceeded");
      context.visit =
          (r.node_visits.contains(definition.id) ? r.node_visits.at(definition.id) : 0) + 1;
      const auto policy = permission(definition, r);
      if (policy.decision == PolicyDecision::Deny)
        throw Error(ErrorCode::Policy, "Operation denied by policy");
      if ((definition.type == "approval" || policy.decision == PolicyDecision::RequireApproval) &&
          !approved(r)) {
        std::lock_guard lock(mutex_);
        context.check();
        wait_approval(r, definition,
                      definition.type == "approval" ? definition.reason : policy.reason);
        co_return;
      }
      if (definition.type == "subpipeline") {
        if (r.child_id.empty()) {
          unsigned depth = 0;
          auto ancestor = r;
          while (!ancestor.parent_id.empty()) {
            ancestor = deps_.storage.get(RecordKind::Run, ancestor.parent_id).get<Run>();
            if (++depth >= 8)
              throw Error(ErrorCode::Execution, "Subpipeline nesting limit exceeded");
          }
          auto registration = deps_.storage.get(RecordKind::Pipeline, definition.binding);
          r.child_id = run(parse_pipeline(registration.at("yaml").get<std::string>(),
                                          {extensions.begin(), extensions.end()}),
                           r.message.payload, r.actor, r.id);
          checkpoint(r, "subpipeline.started");
        }
        for (;;) {
          auto child = deps_.storage.get(RecordKind::Run, r.child_id).get<Run>();
          if (terminal(child.state)) {
            if (child.state != RunState::Completed)
              throw Error(ErrorCode::Execution, "Subpipeline did not complete successfully");
            r.message.payload = child.message.payload;
            break;
          }
          if (child.state == RunState::WaitingApproval || child.state == RunState::Paused) {
            std::lock_guard lock(mutex_);
            transition(r, RunState::Paused, "subpipeline.waiting");
            co_return;
          }
          co_await context.delay(Milliseconds{5});
        }
      }
      auto node = make_node(definition);
      bool completed = false;
      for (unsigned attempt_number = 1; attempt_number <= definition.retry.max_attempts;
           ++attempt_number) {
        context.attempt = attempt_number;
        context.deadline =
            std::min(deadline, std::chrono::steady_clock::now() + definition.timeout.timeout);
        if (definition.type == "tool")
          context.deadline = std::min(context.deadline,
                                      std::chrono::steady_clock::now() +
                                          deps_.tools.get(definition.binding)->metadata().timeout);
        if (definition.type == "agent")
          context.deadline =
              std::min(context.deadline,
                       std::chrono::steady_clock::now() +
                           deps_.providers.get(config_.models.at(definition.binding).provider)
                               ->metadata()
                               .timeout);
        NodeExecution attempt;
        attempt.run_id = r.id;
        attempt.node_id = definition.id;
        attempt.attempt = attempt_number;
        auto started = std::chrono::steady_clock::now();
        checkpoint(r, "node.started", {{RecordKind::Attempt, attempt.id, r.id, Json(attempt)}});
        attempt_recorded = true;
        if (definition.type == "tool")
          transition(r, RunState::WaitingTool, "tool.called");
        if (definition.type == "agent")
          transition(r, RunState::WaitingModel, "model.called");
        std::optional<ErrorCode> failure;
        NodeResult result;
        try {
          auto slot = co_await nodes_.acquire(context);
          result = co_await node->execute(context, r.message);
          context.check();
          if (result.message.payload.dump().size() > max_document_bytes)
            throw Error(ErrorCode::Execution, "Node result exceeds 1 MiB");
        } catch (const Error &e) {
          failure = e.code;
        } catch (...) {
          failure = ErrorCode::Execution;
        }
        attempt.finished_at = timestamp();
        attempt.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (r.state == RunState::WaitingTool || r.state == RunState::WaitingModel)
          transition(r, RunState::Running,
                     failure ? (definition.type == "tool" ? "tool.failed" : "model.failed")
                             : (definition.type == "tool" ? "tool.completed" : "model.completed"));
        if (failure) {
          attempt.state = *failure == ErrorCode::Timeout        ? NodeState::TimedOut
                          : *failure == ErrorCode::Cancellation ? NodeState::Cancelled
                                                                : NodeState::Failed;
          attempt.error = *failure == ErrorCode::Timeout        ? "Node deadline exceeded"
                          : *failure == ErrorCode::Cancellation ? "Node cancelled"
                                                                : "Node execution failed";
          checkpoint(r, "node.failed", {{RecordKind::Attempt, attempt.id, r.id, Json(attempt)}});
          if (*failure == ErrorCode::Cancellation || *failure == ErrorCode::Timeout ||
              attempt_number == definition.retry.max_attempts)
            throw Error(*failure, attempt.error);
          transition(r, RunState::Retrying, "node.retrying");
          context.deadline = deadline;
          co_await context.delay(definition.retry.delay);
          transition(r, RunState::Running, "run.running");
          continue;
        }
        attempt.state = NodeState::Completed;
        ++r.steps;
        ++r.node_visits[definition.id];
        auto parent_message = r.message.id;
        r.message = std::move(result.message);
        r.message.id = uuid();
        r.message.run_id = r.id;
        r.message.pipeline_id = r.pipeline_id;
        r.message.node_id = definition.id;
        r.message.time = timestamp();
        r.message.provenance.push_back(
            {definition.id, "", "", "", "", parent_message, "", timestamp()});
        if (r.message.provenance.size() > 256)
          r.message.provenance.erase(r.message.provenance.begin(),
                                     r.message.provenance.end() - 256);
        std::vector<Record> records{{RecordKind::Attempt, attempt.id, r.id, Json(attempt)},
                                    {RecordKind::Message, r.message.id, r.id, Json(r.message)}};
        if (definition.type == "subpipeline")
          r.child_id.clear();
        bool continuing = false;
        try {
          continuing = advance(r, pipeline, definition, result.condition);
        } catch (...) {
          // Keep the completed node's result and final attempt even when routing fails.
          checkpoint(r, "node.completed", std::move(records));
          throw;
        }
        if (!continuing) {
          transition(r, RunState::Completed, "run.completed", std::move(records));
          co_return;
        }
        checkpoint(r, "node.completed", std::move(records));
        completed = true;
        break;
      }
      if (!completed)
        throw Error(ErrorCode::Execution, "Node did not complete");
    }
  } catch (const Error &e) {
    std::lock_guard lock(mutex_);
    if (!attempt_recorded) {
      NodeExecution attempt;
      attempt.run_id = r.id;
      attempt.node_id = r.active_node;
      attempt.state = e.code == ErrorCode::Timeout        ? NodeState::TimedOut
                      : e.code == ErrorCode::Cancellation ? NodeState::Cancelled
                                                          : NodeState::Failed;
      attempt.finished_at = timestamp();
      attempt.error = "Node preparation failed";
      checkpoint(r, "node.failed", {{RecordKind::Attempt, attempt.id, r.id, Json(attempt)}});
    }
    if (!r.child_id.empty()) {
      auto child = deps_.storage.get(RecordKind::Run, r.child_id).get<Run>();
      if (!terminal(child.state))
        cancel(child.id);
    }
    r.error = e.code == ErrorCode::Timeout        ? "Execution deadline exceeded"
              : e.code == ErrorCode::Cancellation ? "Execution cancelled"
                                                  : "Pipeline execution failed";
    if (!terminal(r.state))
      transition(r,
                 e.code == ErrorCode::Timeout        ? RunState::TimedOut
                 : e.code == ErrorCode::Cancellation ? RunState::Cancelled
                                                     : RunState::Failed,
                 e.code == ErrorCode::Timeout        ? "run.timed_out"
                 : e.code == ErrorCode::Cancellation ? "run.cancelled"
                                                     : "run.failed");
  } catch (...) {
    std::lock_guard lock(mutex_);
    if (!attempt_recorded) {
      NodeExecution attempt;
      attempt.run_id = r.id;
      attempt.node_id = r.active_node;
      attempt.state = NodeState::Failed;
      attempt.finished_at = timestamp();
      attempt.error = "Node preparation failed";
      checkpoint(r, "node.failed", {{RecordKind::Attempt, attempt.id, r.id, Json(attempt)}});
    }
    if (!r.child_id.empty()) {
      auto child = deps_.storage.get(RecordKind::Run, r.child_id).get<Run>();
      if (!terminal(child.state))
        cancel(child.id);
    }
    r.error = "Unexpected pipeline failure";
    if (!terminal(r.state))
      transition(r, RunState::Failed, "run.failed");
  }
}
} // namespace laso
