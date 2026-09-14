#include <algorithm>
#include <laso/pipeline/parser.hpp>
#include <laso/runtime/runtime.hpp>

namespace laso {
struct Runtime::ParallelState {
  explicit ParallelState(std::size_t width) : outputs(width) {}
  std::mutex mutex;
  std::vector<std::optional<Message>> outputs;
  std::size_t completed = 0;
  bool failed = false;
  std::string error;
  ErrorCode code = ErrorCode::Execution;
  std::stop_source stop;
};

Task<void> Runtime::execute_branch(const PipelineDefinition &pipeline, ExecutionToken token,
                                   std::shared_ptr<ParallelState> state,
                                   std::shared_ptr<AsyncLimiter> run_nodes) {
  try {
    Run branch;
    branch.id = token.message.run_id;
    branch.pipeline_id = token.message.pipeline_id;
    branch.definition = pipeline.source;
    branch.active_node = token.node_id;
    branch.message = std::move(token.message);
    branch.frames = std::move(token.frames);
    branch.actor = "parallel";
    for (;;) {
      const auto &definition = pipeline.nodes.at(branch.active_node);
      if (definition.type == "join") {
        const auto index = branch.frames.empty() ? 0U : branch.frames.back().index;
        std::lock_guard lock(state->mutex);
        if (index >= state->outputs.size() || state->outputs[index].has_value())
          throw Error(ErrorCode::Execution, "Parallel branch arrived twice");
        state->outputs[index] = std::move(branch.message);
        ++state->completed;
        co_return;
      }
      ExecutionContext context{branch.id, branch.pipeline_id, definition.id, state->stop.get_token(),
                               std::chrono::steady_clock::now() + definition.timeout.timeout};
      context.visit = branch.node_visits[definition.id] + 1;
      if (definition.type == "tool") {
        const auto metadata = deps_.tools.get(definition.binding)->metadata();
        context.deadline = std::min(context.deadline,
                                    std::chrono::steady_clock::now() + metadata.timeout);
      } else if (definition.type == "agent") {
        const auto binding = config_.models.find(definition.binding);
        if (binding == config_.models.end())
          throw Error(ErrorCode::Provider, "Logical model not configured");
        context.deadline = std::min(
            context.deadline, std::chrono::steady_clock::now() +
                                  deps_.providers.get(binding->second.provider)->metadata().timeout);
      }
      auto node = make_node(definition);
      bool succeeded = false;
      for (unsigned attempt_number = 1; attempt_number <= definition.retry.max_attempts;
           ++attempt_number) {
        context.attempt = attempt_number;
        bool retry = false;
        NodeExecution attempt;
        attempt.run_id = branch.id;
        attempt.node_id = definition.id;
        attempt.attempt = attempt_number;
        deps_.storage.commit({{RecordKind::Attempt, attempt.id, branch.id, Json(attempt)}});
        try {
          auto global_slot = co_await nodes_.acquire(context);
          auto run_slot = co_await run_nodes->acquire(context);
          deps_.schemas.validate(definition.input_schema, branch.message.payload, definition.id,
                                "input");
          auto result = co_await node->execute(context, branch.message);
          context.check();
          deps_.schemas.validate(definition.output_schema, result.message.payload, definition.id,
                                "output");
          attempt.state = NodeState::Completed;
          ++branch.node_visits[definition.id];
          auto parent = branch.message.id;
          branch.message = std::move(result.message);
          branch.message.id = uuid();
          branch.message.run_id = branch.id;
          branch.message.pipeline_id = branch.pipeline_id;
          branch.message.node_id = definition.id;
          branch.message.time = timestamp();
          branch.message.provenance.push_back(
              {definition.id, "", "", "", "", parent, "", timestamp()});
          deps_.storage.commit({{RecordKind::Attempt, attempt.id, branch.id, Json(attempt)},
                                {RecordKind::Message, branch.message.id, branch.id,
                                 Json(branch.message)}});
          std::vector<const EdgeDefinition *> edges;
          for (const auto &edge : pipeline.edges)
            if (edge.from == definition.id &&
                (edge.condition.empty() || edge.condition == result.condition))
              edges.push_back(&edge);
          if (edges.size() != 1)
            throw Error(ErrorCode::Execution, "Parallel branch has ambiguous routing");
          branch.active_node = edges.front()->to;
          succeeded = true;
          break;
        } catch (const Error &error) {
          attempt.state = error.code == ErrorCode::Cancellation ? NodeState::Cancelled
                          : error.code == ErrorCode::Timeout ? NodeState::TimedOut
                                                             : NodeState::Failed;
          attempt.error = attempt.state == NodeState::Cancelled ? "Node cancelled"
                         : attempt.state == NodeState::TimedOut ? "Node deadline exceeded"
                                                                  : error.code == ErrorCode::Validation
                                                                        ? "Schema validation failed"
                                                                        : "Node execution failed";
          deps_.storage.commit({{RecordKind::Attempt, attempt.id, branch.id, Json(attempt)}});
          if (attempt.state == NodeState::Cancelled || attempt.state == NodeState::TimedOut ||
              attempt_number == definition.retry.max_attempts)
            throw;
          retry = true;
        }
        if (retry)
          co_await context.delay(definition.retry.delay);
      }
      if (!succeeded)
        throw Error(ErrorCode::Execution, "Parallel branch did not complete");
    }
  } catch (const Error &error) {
    std::lock_guard lock(state->mutex);
    if (!state->failed) {
      state->failed = true;
      state->error = error.what();
      state->code = error.code;
    }
    ++state->completed;
    state->stop.request_stop();
  } catch (...) {
    std::lock_guard lock(state->mutex);
    if (!state->failed) {
      state->failed = true;
      state->error = "Parallel branch failed";
    }
    ++state->completed;
    state->stop.request_stop();
  }
}

Task<void> Runtime::execute_parallel(Run &run, const PipelineDefinition &pipeline,
                                     std::shared_ptr<AsyncLimiter> run_nodes, std::stop_token parent) {
  std::vector<ExecutionToken> tokens;
  tokens.swap(run.ready);
  auto state = std::make_shared<ParallelState>(tokens.size());
  std::stop_callback parent_stop(parent, [state] { state->stop.request_stop(); });
  for (auto &token : tokens)
    asio::co_spawn(io_, execute_branch(pipeline, std::move(token), state, run_nodes), asio::detached);
  while (true) {
    {
      std::lock_guard lock(state->mutex);
      if (state->completed == state->outputs.size()) {
        if (state->failed)
          throw Error(state->code, state->error);
        break;
      }
    }
    asio::steady_timer timer(co_await asio::this_coro::executor, Milliseconds{2});
    co_await timer.async_wait(asio::use_awaitable);
  }
  run.message.payload = Json::array();
  for (const auto &output : state->outputs) {
    if (!output)
      throw Error(ErrorCode::Execution, "Parallel branch output missing");
    run.message.payload.push_back(output->payload);
    run.message.provenance.insert(run.message.provenance.end(), output->provenance.begin(),
                                  output->provenance.end());
  }
  run.frames.clear();
  run.active_node = pipeline.nodes.at(run.active_node).join;
  run.prepared_join = run.active_node;
  co_return;
}

Task<void> Runtime::execute(Run r, std::stop_token stop) {
  auto extensions = deps_.nodes.names();
  const auto pipeline = parse_pipeline(r.definition, {extensions.begin(), extensions.end()});
  const auto deadline = std::chrono::steady_clock::now() + pipeline.timeout.timeout;
  auto run_nodes = std::make_shared<AsyncLimiter>(config_.max_nodes_per_run);
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
        std::string failure_detail;
        NodeResult result;
        try {
          auto global_slot = co_await nodes_.acquire(context);
          auto run_slot = co_await run_nodes->acquire(context);
          deps_.schemas.validate(definition.input_schema, r.message.payload, definition.id,
                                 "input");
          result = co_await node->execute(context, r.message);
          context.check();
          deps_.schemas.validate(definition.output_schema, result.message.payload, definition.id,
                                 "output");
          if (result.message.payload.dump().size() > max_document_bytes)
            throw Error(ErrorCode::Execution, "Node result exceeds 1 MiB");
        } catch (const Error &e) {
          failure = e.code;
          if (e.code == ErrorCode::Validation && e.details.is_object()) {
            failure_detail = e.what();
            if (e.details.contains("schema"))
              failure_detail += " (schema=" + e.details.at("schema").get<std::string>();
            if (e.details.contains("direction"))
              failure_detail += ", direction=" + e.details.at("direction").get<std::string>();
            if (e.details.contains("instance_path"))
              failure_detail += ", instance_path=" + e.details.at("instance_path").get<std::string>();
            failure_detail += ")";
          }
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
                                                                : failure_detail.empty() ? "Node execution failed"
                                                                                          : failure_detail;
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
          if (definition.type == "parallel") {
            bool approval_branch = false;
            for (const auto &edge : pipeline.edges)
              if (edge.from == definition.id && pipeline.nodes.at(edge.to).type == "approval")
                approval_branch = true;
            continuing = advance(r, pipeline, definition, result.condition);
            if (approval_branch) {
              // Approval branches need the existing durable pause/resume protocol.
              continuing = next_ready(r);
            } else {
              co_await execute_parallel(r, pipeline, run_nodes, stop);
              result.message = r.message;
            }
          } else {
            continuing = advance(r, pipeline, definition, result.condition);
          }
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
