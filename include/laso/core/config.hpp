#pragma once
#include <cstdint>
#include <filesystem>
#include <laso/policies/policy.hpp>
#include <laso/providers/provider.hpp>

namespace laso {
struct EventSourceConfig {
  std::string plugin, component, schema;
  bool enabled = false;
  Json config = Json::object();
};
struct WorkerConfig {
  std::string plugin, component, event_schema;
  bool enabled = true;
  Json config = Json::object();
};
struct Config {
  std::filesystem::path data_dir = ".laso", db_path;
  std::string storage_backend = "sqlite";
  std::string postgres_dsn, postgres_schema = "public";
  std::vector<std::filesystem::path> plugin_dirs;
  std::vector<std::filesystem::path> schema_roots;
  std::string api_host = "127.0.0.1", log_level = "info", local_openai_endpoint;
  unsigned api_port = 8080, workers = 2, max_runs = 16, max_nodes = 32, max_nodes_per_run = 8,
           max_models = 4, max_tools = 8, max_subpipeline_depth = 16,
           max_pending_scheduler_launches = 128, max_event_trigger_depth = 16,
           max_event_trigger_deliveries = 1024, max_worker_jobs = 32,
           max_worker_jobs_per_worker = 16;
  // Zero disables a budget. Token and cost budgets accumulate per run.
  std::uint64_t max_worker_wall_time_ms = 0, max_worker_tokens_per_run = 0;
  double max_worker_cost_units_per_run = 0.0;
  bool json_logs = false, allow_network = false, allow_remote_api = false;
  std::map<std::string, EventSourceConfig> event_sources;
  std::map<std::string, WorkerConfig> worker_plugins;
  std::map<std::string, ModelBinding> models{{"research", {"mock", "mock-v1"}},
                                             {"reviewer", {"mock", "mock-v1"}}};
  std::vector<PolicyRule> rules;
  void validate();
};
Config load_config(const std::filesystem::path &file = {},
                   const std::map<std::string, std::string> &overrides = {});
void configure_logging(const Config &);
void log_event(const Event &event);
void log_diagnostic(const std::string &event, const Json &safe_metadata = Json::object());
} // namespace laso
