#pragma once
#include <functional>
#include <laso/core/config.hpp>
#include <laso/events/events.hpp>
#include <laso/nodes/node.hpp>
#include <laso/schema/validator.hpp>
#include <laso/storage/storage.hpp>
#include <memory>
#include <mutex>
#include <set>

namespace laso {
struct RuntimeDependencies {
  Storage &storage;
  EventBus &events;
  ProviderRegistry &providers;
  ToolRegistry &tools;
  FunctionRegistry &functions;
  NodeRegistry &nodes;
  Policy &policy;
  SchemaValidator &schemas;
  std::function<PipelineDefinition(const std::string &)> resolve_pipeline;
};
class Runtime {
public:
  Runtime(asio::io_context &io, Config config, RuntimeDependencies dependencies);
  ~Runtime();
  // Returns immediately. Execution is scheduled on the bounded Asio executor.
  std::string run(const PipelineDefinition &, Json input = Json::object(),
                  std::string actor = "local", std::string parent_id = "",
                  std::string parent_node_id = "", unsigned subpipeline_depth = 0,
                  std::string parent_message_id = "");
  void resume(const std::string &id);
  void cancel(const std::string &id);
  void decide(const std::string &approval_id, bool approve, const std::string &actor,
              const std::string &comment);
  void shutdown();
  bool idle() const;

private:
  struct ParallelState;
  asio::io_context &io_;
  Config config_;
  RuntimeDependencies deps_;
  AsyncLimiter nodes_, models_, tools_;
  mutable std::recursive_mutex mutex_;
  std::map<std::string, std::stop_source> active_;
  bool stopping_ = false;
  Task<void> execute(Run run, std::stop_token stop);
  Task<void> execute_branch(const PipelineDefinition &, ExecutionToken,
                            std::shared_ptr<ParallelState>, std::shared_ptr<AsyncLimiter>,
                            std::chrono::steady_clock::time_point, unsigned);
  Task<void> execute_parallel(Run &, const PipelineDefinition &, std::shared_ptr<AsyncLimiter>,
                              std::stop_token, std::chrono::steady_clock::time_point, unsigned);
  void schedule(Run run);
  void cancel_locked(const std::string &, std::set<std::string> &);
  void transition(Run &, RunState, const std::string &event, std::vector<Record> records = {});
  void checkpoint(Run &, const std::string &event, std::vector<Record> records = {});
  std::unique_ptr<Node> make_node(const NodeDefinition &);
  PolicyResult permission(const NodeDefinition &, const Run &) const;
  bool approved(const Run &) const;
  void wait_approval(Run &, const NodeDefinition &, const std::string &reason);
  bool advance(Run &, const PipelineDefinition &, const NodeDefinition &,
               const std::string &condition);
  bool prepare_join(Run &, const NodeDefinition &);
  bool next_ready(Run &);
  void finish_parent(const Run &);
};
} // namespace laso
