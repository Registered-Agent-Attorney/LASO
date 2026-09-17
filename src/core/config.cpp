#include "../pipeline/yaml.hpp"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <laso/core/config.hpp>
#include <laso/pipeline/parser.hpp>
#include <regex>
#include <set>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

namespace laso {
void Config::validate() {
  if (data_dir.empty())
    throw Error(ErrorCode::Configuration, "Data directory is empty");
  if (storage_backend != "sqlite" && storage_backend != "postgres")
    throw Error(ErrorCode::Configuration, "Unsupported storage backend");
  if (storage_backend == "postgres" && postgres_dsn.empty())
    throw Error(ErrorCode::Configuration, "PostgreSQL DSN is required");
  if (storage_backend == "postgres" &&
      (postgres_schema.empty() || postgres_schema.size() > 63 ||
       !std::isalpha(static_cast<unsigned char>(postgres_schema.front()))))
    throw Error(ErrorCode::Configuration, "Invalid PostgreSQL schema");
  if (storage_backend == "postgres")
    for (const auto ch : postgres_schema)
      if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
        throw Error(ErrorCode::Configuration, "Invalid PostgreSQL schema");
  if (db_path.empty())
    db_path = data_dir / "laso.db";
  if (api_port == 0 || api_port > 65535 || workers == 0 || workers > 64 || max_runs == 0 ||
      max_runs > 1024 || max_nodes == 0 || max_nodes > 4096 || max_nodes_per_run == 0 ||
      max_nodes_per_run > max_nodes || max_models == 0 || max_models > 1024 || max_tools == 0 ||
      max_tools > 1024 || max_subpipeline_depth == 0 || max_subpipeline_depth > 64 ||
      max_pending_scheduler_launches == 0 || max_pending_scheduler_launches > 4096 ||
      max_event_trigger_depth == 0 || max_event_trigger_depth > 64 ||
      max_event_trigger_deliveries == 0 || max_event_trigger_deliveries > 100000 ||
      max_worker_jobs == 0 || max_worker_jobs > 4096 || max_worker_jobs_per_worker == 0 ||
      max_worker_jobs_per_worker > max_worker_jobs || max_worker_wall_time_ms > 1000000000000000ULL ||
      max_worker_tokens_per_run > 1000000000000000ULL ||
      !std::isfinite(max_worker_cost_units_per_run) || max_worker_cost_units_per_run < 0 ||
      max_worker_cost_units_per_run > 1000000000000000.0)
    throw Error(ErrorCode::Configuration, "Invalid port or concurrency limit");
  if (api_host != "127.0.0.1" && api_host != "::1" && !allow_remote_api)
    throw Error(ErrorCode::Configuration, "Non-loopback API requires allow_remote_api=true");
  if (log_level != "debug" && log_level != "info" && log_level != "warn" && log_level != "error")
    throw Error(ErrorCode::Configuration, "Invalid log level");
  if (event_sources.size() > 64)
    throw Error(ErrorCode::Configuration, "Too many event sources");
  static const std::regex source_id_pattern("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}");
  for (const auto &[id, source] : event_sources) {
    if (!std::regex_match(id, source_id_pattern) || source.plugin.empty() ||
        source.plugin.size() > 128 || source.component.size() > 128 || source.schema.size() > 512 ||
        source.config.dump().size() > std::size_t{1024} * 1024)
      throw Error(ErrorCode::Configuration, "Invalid event source configuration");
  }
  if (worker_plugins.size() > 64)
    throw Error(ErrorCode::Configuration, "Too many workers");
  for (const auto &[id, worker] : worker_plugins) {
    if (!std::regex_match(id, source_id_pattern) || worker.plugin.empty() ||
        worker.plugin.size() > 128 || worker.component.size() > 128 ||
        worker.event_schema.size() > 512 || worker.config.dump().size() > std::size_t{1024} * 1024)
      throw Error(ErrorCode::Configuration, "Invalid worker configuration");
  }
}
Config load_config(const std::filesystem::path &supplied,
                   const std::map<std::string, std::string> &overrides) {
  Config c;
  auto path = supplied;
  if (path.empty())
    if (auto *env = std::getenv("LASO_CONFIG"))
      path = env;
  std::map<std::string, std::string> values;
  if (!path.empty()) {
    try {
      const auto n = detail::load_safe_yaml(read_document(path));
      if (!n.IsMap())
        throw Error(ErrorCode::Configuration, "Config must be a map");
      std::set<std::string> seen;
      for (const auto &pair : n) {
        auto key = pair.first.as<std::string>();
        if (!seen.insert(key).second)
          throw Error(ErrorCode::Configuration, "Duplicate configuration field");
        if (key == "models") {
          c.models.clear();
          for (const auto &model : pair.second) {
            auto name = model.first.as<std::string>();
            if (!c.models
                     .emplace(name, ModelBinding{model.second["provider"].as<std::string>(),
                                                 model.second["model"].as<std::string>()})
                     .second)
              throw Error(ErrorCode::Configuration, "Duplicate logical model");
          }
        } else if (key == "plugin_dirs") {
          if (!pair.second.IsSequence())
            throw Error(ErrorCode::Configuration, "plugin_dirs must be a sequence");
          for (const auto &dir : pair.second)
            c.plugin_dirs.emplace_back(dir.as<std::string>());
        } else if (key == "schema_roots") {
          if (!pair.second.IsSequence())
            throw Error(ErrorCode::Configuration, "schema_roots must be a sequence");
          for (const auto &root : pair.second)
            c.schema_roots.emplace_back(root.as<std::string>());
        } else if (key == "policies") {
          for (const auto &rule : pair.second) {
            auto decision = rule["decision"].as<std::string>();
            if (decision != "allow" && decision != "deny" && decision != "approval")
              throw Error(ErrorCode::Configuration, "Unknown policy decision");
            c.rules.push_back({rule["resource"].as<std::string>(),
                               decision == "deny"       ? PolicyDecision::Deny
                               : decision == "approval" ? PolicyDecision::RequireApproval
                                                        : PolicyDecision::Allow});
          }
        } else if (key == "event_sources") {
          if (!pair.second.IsMap())
            throw Error(ErrorCode::Configuration, "event_sources must be a map");
          for (const auto &source : pair.second) {
            const auto id = source.first.as<std::string>();
            if (!std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")))
              throw Error(ErrorCode::Configuration, "Invalid event source id");
            const auto node = source.second;
            if (!node.IsMap())
              throw Error(ErrorCode::Configuration, "Event source must be a map");
            std::set<std::string> fields;
            for (const auto &field : node)
              if (!fields.insert(field.first.as<std::string>()).second)
                throw Error(ErrorCode::Configuration, "Duplicate event source field");
            for (const auto &field : fields)
              if (field != "plugin" && field != "component" && field != "schema" &&
                  field != "enabled" && field != "config")
                throw Error(ErrorCode::Configuration, "Unknown event source field");
            EventSourceConfig cfg;
            if (!node["plugin"])
              throw Error(ErrorCode::Configuration, "Event source plugin is required");
            cfg.plugin = node["plugin"].as<std::string>();
            if (node["component"])
              cfg.component = node["component"].as<std::string>();
            if (node["schema"])
              cfg.schema = node["schema"].as<std::string>();
            if (node["enabled"])
              cfg.enabled = node["enabled"].as<bool>();
            if (node["config"])
              cfg.config = detail::yaml_value(node["config"]);
            if (!c.event_sources.emplace(id, std::move(cfg)).second)
              throw Error(ErrorCode::Configuration, "Duplicate event source id");
          }
        } else if (key == "worker_plugins") {
          if (!pair.second.IsMap())
            throw Error(ErrorCode::Configuration, "worker_plugins must be a map");
          for (const auto &worker : pair.second) {
            const auto id = worker.first.as<std::string>();
            if (!std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")))
              throw Error(ErrorCode::Configuration, "Invalid worker id");
            const auto node = worker.second;
            if (!node.IsMap())
              throw Error(ErrorCode::Configuration, "Worker must be a map");
            std::set<std::string> fields;
            for (const auto &field : node)
              if (!fields.insert(field.first.as<std::string>()).second)
                throw Error(ErrorCode::Configuration, "Duplicate worker field");
            for (const auto &field : fields)
              if (field != "plugin" && field != "component" && field != "event_schema" &&
                  field != "enabled" && field != "config")
                throw Error(ErrorCode::Configuration, "Unknown worker field");
            WorkerConfig cfg;
            if (!node["plugin"])
              throw Error(ErrorCode::Configuration, "Worker plugin is required");
            cfg.plugin = node["plugin"].as<std::string>();
            if (node["component"])
              cfg.component = node["component"].as<std::string>();
            if (node["event_schema"])
              cfg.event_schema = node["event_schema"].as<std::string>();
            if (node["enabled"])
              cfg.enabled = node["enabled"].as<bool>();
            if (node["config"])
              cfg.config = detail::yaml_value(node["config"]);
            if (!c.worker_plugins.emplace(id, std::move(cfg)).second)
              throw Error(ErrorCode::Configuration, "Duplicate worker id");
          }
        } else
          values[key] = pair.second.as<std::string>();
      }
    } catch (const YAML::Exception &) {
      throw Error(ErrorCode::Configuration, "Invalid configuration YAML");
    }
  }
  for (auto name : {"DATA_DIR",
                    "DB_PATH",
                    "STORAGE_BACKEND",
                    "POSTGRES_DSN",
                    "POSTGRES_SCHEMA",
                    "PLUGIN_DIR",
                    "LOG_LEVEL",
                    "API_HOST",
                    "API_PORT",
                    "WORKERS",
                    "MAX_RUNS",
                    "MAX_NODES",
                    "MAX_NODES_PER_RUN",
                    "MAX_MODELS",
                    "MAX_TOOLS",
                    "MAX_SUBPIPELINE_DEPTH",
                    "MAX_PENDING_SCHEDULER_LAUNCHES",
                    "MAX_EVENT_TRIGGER_DEPTH",
                    "MAX_EVENT_TRIGGER_DELIVERIES",
                    "MAX_WORKER_JOBS",
                    "MAX_WORKER_JOBS_PER_WORKER",
                    "MAX_WORKER_WALL_TIME_MS",
                    "MAX_WORKER_TOKENS_PER_RUN",
                    "MAX_WORKER_COST_UNITS_PER_RUN",
                    "JSON_LOGS",
                    "ALLOW_NETWORK",
                    "ALLOW_REMOTE_API",
                    "LOCAL_OPENAI_ENDPOINT"}) {
    auto variable = std::string("LASO_") + name;
    if (auto *v = std::getenv(variable.c_str())) {
      std::string key = name;
      for (auto &ch : key)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      values[key] = v;
    }
  }
  for (const auto &[k, v] : overrides)
    values[k] = v;
  auto boolean = [](const std::string &s) {
    if (s == "true")
      return true;
    if (s == "false")
      return false;
    throw Error(ErrorCode::Configuration, "Boolean must be true or false");
  };
  auto integer = [](const std::string &s) {
    try {
      std::size_t end = 0;
      auto result = std::stoul(s, &end);
      if (end != s.size() || result > 100000)
        throw std::out_of_range("limit");
      return static_cast<unsigned>(result);
    } catch (...) {
      throw Error(ErrorCode::Configuration, "Invalid numeric configuration");
    }
  };
  auto uint64 = [](const std::string &s) {
    try {
      std::size_t end = 0;
      const auto result = std::stoull(s, &end);
      if (end != s.size() || result > 1000000000000000ULL)
        throw std::out_of_range("limit");
      return static_cast<std::uint64_t>(result);
    } catch (...) {
      throw Error(ErrorCode::Configuration, "Invalid numeric configuration");
    }
  };
  auto real = [](const std::string &s) {
    try {
      std::size_t end = 0;
      const auto result = std::stod(s, &end);
      if (end != s.size() || !std::isfinite(result) || result < 0 || result > 1e15)
        throw std::out_of_range("limit");
      return result;
    } catch (...) {
      throw Error(ErrorCode::Configuration, "Invalid numeric configuration");
    }
  };
  for (const auto &[k, v] : values) {
    if (k == "data_dir")
      c.data_dir = v;
    else if (k == "storage_backend")
      c.storage_backend = v;
    else if (k == "postgres_dsn")
      c.postgres_dsn = v;
    else if (k == "postgres_schema")
      c.postgres_schema = v;
    else if (k == "db_path")
      c.db_path = v;
    else if (k == "plugin_dir")
      c.plugin_dirs = {std::filesystem::path(v)};
    else if (k == "api_host")
      c.api_host = v;
    else if (k == "api_port")
      c.api_port = integer(v);
    else if (k == "log_level")
      c.log_level = v;
    else if (k == "local_openai_endpoint")
      c.local_openai_endpoint = v;
    else if (k == "json_logs")
      c.json_logs = boolean(v);
    else if (k == "allow_network")
      c.allow_network = boolean(v);
    else if (k == "allow_remote_api")
      c.allow_remote_api = boolean(v);
    else if (k == "workers")
      c.workers = integer(v);
    else if (k == "max_runs")
      c.max_runs = integer(v);
    else if (k == "max_nodes")
      c.max_nodes = integer(v);
    else if (k == "max_nodes_per_run")
      c.max_nodes_per_run = integer(v);
    else if (k == "max_models")
      c.max_models = integer(v);
    else if (k == "max_tools")
      c.max_tools = integer(v);
    else if (k == "max_subpipeline_depth")
      c.max_subpipeline_depth = integer(v);
    else if (k == "max_pending_scheduler_launches")
      c.max_pending_scheduler_launches = integer(v);
    else if (k == "max_event_trigger_depth")
      c.max_event_trigger_depth = integer(v);
    else if (k == "max_event_trigger_deliveries")
      c.max_event_trigger_deliveries = integer(v);
    else if (k == "max_worker_jobs")
      c.max_worker_jobs = integer(v);
    else if (k == "max_worker_jobs_per_worker")
      c.max_worker_jobs_per_worker = integer(v);
    else if (k == "max_worker_wall_time_ms")
      c.max_worker_wall_time_ms = uint64(v);
    else if (k == "max_worker_tokens_per_run")
      c.max_worker_tokens_per_run = uint64(v);
    else if (k == "max_worker_cost_units_per_run")
      c.max_worker_cost_units_per_run = real(v);
    else
      throw Error(ErrorCode::Configuration, "Unknown configuration field");
  }
  c.validate();
  return c;
}
void configure_logging(const Config &c) {
  auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  spdlog::set_default_logger(std::make_shared<spdlog::logger>("laso", sink));
  spdlog::set_level(spdlog::level::from_str(c.log_level));
  spdlog::set_pattern(c.json_logs ? "%v" : "[%H:%M:%S] [%l] %v");
}
void log_event(const Event &e) {
  // Payloads, prompts and arbitrary exception messages are deliberately excluded.
  spdlog::info("{}", Json{{"timestamp", e.time},
                          {"severity", "info"},
                          {"event", e.type},
                          {"run_id", e.run_id},
                          {"pipeline_id", e.pipeline_id},
                          {"node_id", e.node_id},
                          {"provider", e.metadata.value("provider", std::string{})},
                          {"model", e.metadata.value("model", std::string{})},
                          {"tool", e.metadata.value("tool", std::string{})},
                          {"plugin", e.metadata.value("plugin", std::string{})}}
                         .dump());
}
void log_diagnostic(const std::string &event, const Json &safe_metadata) {
  spdlog::info("{}", Json{{"timestamp", timestamp()},
                          {"severity", "info"},
                          {"event", event},
                          {"metadata", safe_metadata}}
                         .dump());
}
} // namespace laso
