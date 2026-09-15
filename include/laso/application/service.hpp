#pragma once
#include <laso/artifacts/artifacts.hpp>
#include <laso/plugins/loader.hpp>
#include <laso/runtime/runtime.hpp>
#include <laso/scheduler/scheduler.hpp>
#include <laso/schema/validator.hpp>
#include <laso/security/security.hpp>

namespace laso {
class Service {
public:
  Service(asio::io_context &, Config);
  const Config &config() const {
    return config_;
  }
  Json register_pipeline(const std::string &yaml);
  std::string start(const std::string &name_or_path, const Json &input = Json::object(),
                    const std::string &actor = "local", bool allow_file = false);
  Json run_view(const std::string &id) const;
  Json get(RecordKind kind, const std::string &id) const {
    return storage_.get(kind, id);
  }
  std::vector<Json> list(RecordKind kind, const std::string &run_id = "", std::size_t limit = 1000,
                         std::size_t offset = 0) const {
    return storage_.list(kind, run_id, limit, offset);
  }
  Json providers() const;
  Json tools() const;
  Json plugins() const;
  Runtime &runtime() {
    return runtime_;
  }
  FunctionRegistry &functions() {
    return functions_;
  }
  ToolRegistry &tool_registry() {
    return tools_;
  }
  ProviderRegistry &provider_registry() {
    return providers_;
  }
  NodeRegistry &node_registry() {
    return nodes_;
  }
  EventBus &event_bus() {
    return events_;
  }
  ArtifactStore &artifacts() {
    return artifacts_;
  }
  Scheduler &scheduler() {
    return scheduler_;
  }
  void shutdown();

private:
  Config config_;
  ProcessLease lease_;
  SQLiteStorage storage_;
  InProcessEventBus events_;
  ProviderRegistry providers_;
  ToolRegistry tools_;
  FunctionRegistry functions_;
  NodeRegistry nodes_;
  PolicyEngine policy_;
  SchemaValidator schemas_;
  PluginLoader plugins_;
  Runtime runtime_;
  LocalArtifactStore artifacts_;
  LocalScheduler scheduler_;
  Json pipeline_record(const std::string &reference) const;
  PipelineDefinition resolve_pipeline(const std::string &reference) const;
  void recover_history();
};
} // namespace laso
