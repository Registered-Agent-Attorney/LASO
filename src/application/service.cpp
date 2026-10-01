#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <laso/application/service.hpp>
#include <laso/pipeline/parser.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace laso {
namespace {
Config checked(Config c) {
  c.validate();
  if (c.execution_mode != "single" && c.execution_mode != "multi_instance")
    throw Error(ErrorCode::Configuration, "Unsupported LASO execution mode");
  return c;
}
std::vector<Json> all_pipeline_records(const Storage &storage) {
  constexpr std::size_t page_size = 10000;
  std::vector<Json> result;
  for (std::size_t offset = 0;;) {
    auto page = storage.list(RecordKind::Pipeline, "", page_size, offset);
    result.insert(result.end(), page.begin(), page.end());
    if (page.size() < page_size)
      return result;
    if (offset > std::numeric_limits<std::size_t>::max() - page_size)
      throw Error(ErrorCode::Storage, "Pipeline registry is too large to inspect");
    offset += page_size;
  }
}
std::string maintenance_mode_name(MaintenanceMode mode) {
  switch (mode) {
  case MaintenanceMode::Active:
    return "active";
  case MaintenanceMode::Draining:
    return "draining";
  case MaintenanceMode::Maintenance:
    return "maintenance";
  }
  return "active";
}
MaintenanceMode parse_maintenance_mode(const std::string &value) {
  if (value == "active")
    return MaintenanceMode::Active;
  if (value == "draining")
    return MaintenanceMode::Draining;
  if (value == "maintenance")
    return MaintenanceMode::Maintenance;
  throw Error(ErrorCode::Configuration, "Invalid maintenance state; refusing startup");
}
void durable_replace(const std::filesystem::path &path, const std::string &contents) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error)
    throw Error(ErrorCode::Storage, "Maintenance state could not be persisted");
  auto temporary = path;
  temporary += "." + uuid() + ".tmp";
  const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0)
    throw Error(ErrorCode::Storage, "Maintenance state could not be persisted");
  bool open = true;
  try {
    std::size_t written = 0;
    while (written < contents.size()) {
      const auto count = ::write(descriptor, contents.data() + written, contents.size() - written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        throw Error(ErrorCode::Storage, "Maintenance state could not be persisted");
      written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0)
      throw Error(ErrorCode::Storage, "Maintenance state could not be persisted");
    open = false;
    if (::rename(temporary.c_str(), path.c_str()) != 0)
      throw Error(ErrorCode::Storage, "Maintenance state could not be persisted");
    const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
      (void)::fsync(directory);
      (void)::close(directory);
    }
  } catch (...) {
    if (open)
      (void)::close(descriptor);
    (void)::unlink(temporary.c_str());
    throw;
  }
}
std::string definition_fingerprint(const std::string &text) {
  // This is an identity aid, not a security primitive.  The immutable source
  // remains in the record and is compared on an idempotent registration.
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto byte : text) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}
bool same_source(const Json &record, const std::string &yaml) {
  return record.contains("yaml") && record.at("yaml").is_string() &&
         record.at("yaml").get<std::string>() == yaml;
}
std::unique_ptr<Coordination> make_coordination(const Config &config,
                                                const std::string &instance_id) {
  if (config.execution_mode != "multi_instance")
    return nullptr;
  return create_coordination(
      {config.postgres_dsn, config.postgres_schema, config.postgres_pool_min_connections,
       config.postgres_pool_max_connections, config.postgres_pool_acquisition_timeout_ms,
       config.instance_stale_after_ms},
      instance_id);
}

std::unique_ptr<ArtifactStore> make_artifact_store(const Config &config, Storage &storage) {
  const auto root =
      config.artifact_root.empty() ? config.data_dir / "artifacts" : config.artifact_root;
  const ArtifactStoreLimits limits{config.max_artifact_bytes, config.max_artifact_temp_bytes,
                                   config.artifact_cleanup_grace_seconds};
  if (config.artifact_backend == "s3") {
#ifdef LASO_HAS_S3
    return std::make_unique<S3ArtifactStore>(
        S3ArtifactStoreConfig{config.artifact_s3_endpoint, config.artifact_s3_bucket,
                              config.artifact_s3_region, config.artifact_s3_prefix,
                              root / "scratch", config.artifact_s3_connect_timeout_ms,
                              config.artifact_s3_request_timeout_ms, config.artifact_s3_max_retries,
                              config.artifact_s3_path_style, config.artifact_s3_allow_http,
                              config.artifact_s3_ca_file},
        storage, limits);
#else
    throw Error(ErrorCode::Configuration,
                "S3 artifact storage requires a build with LASO_ENABLE_S3=ON");
#endif
  }
  if (!config.artifact_service_url.empty())
    return std::make_unique<RemoteArtifactStore>(config.artifact_service_url,
                                                 config.artifact_service_token, root / "cache",
                                                 storage, limits);
  return std::make_unique<LocalArtifactStore>(root, storage, limits);
}
} // namespace
Service::Service(asio::io_context &io, Config config)
    : config_(checked(std::move(config))), instance_id_(generate_service_instance_id()),
      maintenance_state_path_(config_.data_dir / "operator-maintenance.json"),
      storage_(create_storage(
          {config_.postgres_dsn, config_.postgres_schema, config_.postgres_pool_min_connections,
           config_.postgres_pool_max_connections, config_.postgres_pool_acquisition_timeout_ms,
           config_.execution_mode == "multi_instance"})),
      coordination_(make_coordination(config_, instance_id_)),
      policy_(config_.rules, config_.allow_network, config_.allow_remote_workers),
      schemas_(config_.schema_roots),
      ingress_(*storage_, events_, schemas_, config_.max_event_trigger_depth,
               config_.max_pending_scheduler_launches, 32),
      worker_manager_(std::make_shared<WorkerManager>(
          *storage_, worker_registry_, policy_, config_.max_worker_jobs,
          config_.max_worker_jobs_per_worker, 16, config_.max_worker_wall_time_ms,
          config_.max_worker_tokens_per_run, config_.max_worker_cost_units_per_run)),
      plugins_(
          tools_, providers_, worker_registry_,
          [this](const std::string &source, const std::string &plugin, const std::string &component,
                 const std::string &schema, const std::string &event_json) {
            return ingress_.submit(source, plugin, component, schema, event_json);
          },
          [this](const EventSourceInfo &info) {
            storage_->commit({{RecordKind::EventSource, info.id, "", Json(info)}});
          },
          [this](const std::string &id) -> std::optional<EventSourceInfo> {
            try {
              return storage_->get(RecordKind::EventSource, id).get<EventSourceInfo>();
            } catch (const Error &error) {
              if (error.code == ErrorCode::NotFound)
                return std::nullopt;
              throw;
            }
          }),
      artifacts_(make_artifact_store(config_, *storage_)),
      runtime_(io, config_,
               {*storage_, events_, providers_, context_reducers_, tools_, functions_, nodes_,
                policy_, schemas_, worker_manager_,
                [this](const std::string &reference) { return resolve_pipeline(reference); },
                coordination_.get(), artifacts_.get(), instance_id_,
                config_.data_dir / "distributed-workspaces"}),
      scheduler_(
          io, *storage_,
          [this](const LaunchRequest &request) {
            const auto reference =
                pipeline_reference(request.pipeline_id, request.pipeline_version);
            const auto actor = request.origin.value("initiation_type", std::string{}) == "event"
                                   ? "event-trigger"
                                   : "scheduler";
            return start(reference, request.input, actor, false, request.origin);
          },
          [this](const Event &event) {
            events_.publish(event);
            log_event(event);
          },
          std::make_shared<SystemClock>(), config_.max_pending_scheduler_launches,
          config_.max_event_trigger_depth, config_.max_event_trigger_deliveries) {
  load_maintenance_state();
  configure_logging(config_);
  context_reducers_.add("recent-turns", std::make_shared<RecentTurnsContextReducer>());
  providers_.add("mock", std::make_shared<MockModelProvider>());
  if (!config_.local_openai_endpoint.empty())
    providers_.add("local-openai",
                   std::make_shared<LocalOpenAICompatibleProvider>(config_.local_openai_endpoint));
  tools_.add("echo", std::make_shared<EchoTool>());
  register_functions(functions_);
  for (const auto &[id, process_config] : config_.process_workers) {
    auto transport = std::make_shared<ProcessWorkerTransport>(id, process_config);
    worker_registry_.add(id, transport);
    process_workers_.push_back(transport);
    transport->set_interaction_handler(
        [manager = worker_manager_](const WorkerInteractionRequest &request) {
          return manager->handle_interaction(request);
        });
    try {
      transport->start();
    } catch (const std::exception &error) {
      // A disconnected or misconfigured endpoint is degraded worker health,
      // not a control-plane startup failure. Keep it registered so operators
      // can see the failure and later submissions can retry its supervised
      // transport without restarting Core.
      log_diagnostic("worker.initial_start_failed",
                     {{"worker_id", id}, {"error", error.what()}});
    }
  }
  plugins_.discover(config_.plugin_dirs, config_.event_sources, config_.worker_plugins);
  for (const auto &source : plugins_.event_sources())
    if (!source.value("event_schema", std::string{}).empty())
      schemas_.validate_declaration(source.at("event_schema").get<std::string>());
  for (const auto &worker : plugins_.workers())
    if (!worker.value("event_schema", std::string{}).empty())
      schemas_.validate_declaration(worker.at("event_schema").get<std::string>());
  recover_history();
  runtime_.start_distributed();
  events_.subscribe(worker_manager_);
  events_.subscribe(scheduler_.event_subscriber());
  scheduler_.start();
  plugins_.start_event_sources();
  plugins_.start_workers();
}
void Service::load_maintenance_state() {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(maintenance_state_path_, error);
  if (!error && std::filesystem::is_symlink(status))
    throw Error(ErrorCode::Configuration, "Invalid maintenance state; refusing startup");
  if (error == std::errc::no_such_file_or_directory) {
    runtime_.set_maintenance_mode(MaintenanceMode::Active);
    return;
  }
  if (error)
    throw Error(ErrorCode::Storage, "Maintenance state could not be read");
  const auto size = std::filesystem::file_size(maintenance_state_path_, error);
  if (error || size > 65536)
    throw Error(ErrorCode::Configuration, "Invalid maintenance state; refusing startup");
  std::ifstream input(maintenance_state_path_, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof())
    throw Error(ErrorCode::Storage, "Maintenance state could not be read");
  try {
    const auto value = Json::parse(text);
    if (value.value("version", 0U) != 1)
      throw Error(ErrorCode::Configuration, "Invalid maintenance state; refusing startup");
    maintenance_mode_ = parse_maintenance_mode(value.value("state", std::string{}));
    maintenance_started_at_ = value.value("since", std::string{});
    maintenance_actor_ = value.value("actor", std::string{});
    if (maintenance_actor_.size() > 128 || maintenance_started_at_.size() > 64)
      throw Error(ErrorCode::Configuration, "Invalid maintenance state; refusing startup");
    const auto events = value.value("events", Json::array());
    if (!events.is_array() || events.size() > 128)
      throw Error(ErrorCode::Configuration, "Invalid maintenance state; refusing startup");
    operator_events_.assign(events.begin(), events.end());
    (void)::chmod(maintenance_state_path_.c_str(), 0600);
    runtime_.set_maintenance_mode(maintenance_mode_);
  } catch (const Error &) {
    throw;
  } catch (...) {
    throw Error(ErrorCode::Configuration, "Invalid maintenance state; refusing startup");
  }
}
void Service::persist_maintenance_state(MaintenanceMode mode, const std::string &started_at,
                                        const std::string &actor, Json event) {
  auto events = operator_events_;
  events.push_back(std::move(event));
  if (events.size() > 128)
    events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(events.size() - 128));
  Json value{{"version", 1},
             {"state", maintenance_mode_name(mode)},
             {"since", started_at},
             {"actor", actor},
             {"events", events}};
  durable_replace(maintenance_state_path_, value.dump());
  operator_events_ = std::move(events);
  maintenance_mode_ = mode;
  maintenance_started_at_ = started_at;
  maintenance_actor_ = actor;
}
void Service::require_operator_actor(const Actor &actor) const {
  if (!actor.authenticated || actor.id.empty() || actor.id.size() > 128 ||
      (actor.role != "operator" && actor.role != "admin"))
    throw Error(ErrorCode::Policy, "Operator access required");
  const auto valid = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '@' || c == ':' || c == '-';
  };
  if (!std::all_of(actor.id.begin(), actor.id.end(), valid) ||
      !std::isalnum(static_cast<unsigned char>(actor.id.front())))
    throw Error(ErrorCode::Policy, "Operator access required");
}
std::string Service::begin_operator_action(const Actor &actor, const std::string &action,
                                           const std::string &target_type,
                                           const std::string &target_id) {
  require_operator_actor(actor);
  const auto valid_identifier = [](const std::string &value) {
    return !value.empty() && value.size() <= 128 &&
           std::isalnum(static_cast<unsigned char>(value.front())) &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
             return std::isalnum(c) || c == '.' || c == '_' || c == '@' || c == ':' || c == '-';
           });
  };
  const auto safe_target =
      target_id.empty() || valid_identifier(target_id) ? target_id : std::string{};
  const auto operation_id = uuid();
  storage_->append_operator_audit_event({uuid(), operation_id, actor.id, actor.role, action,
                                         target_type, safe_target, "requested", 0});
  return operation_id;
}
void Service::complete_operator_action(const Actor &actor, const std::string &operation_id,
                                       const std::string &action, const std::string &target_type,
                                       const std::string &target_id, const std::string &outcome,
                                       unsigned http_status) {
  require_operator_actor(actor);
  const auto valid_identifier = [](const std::string &value) {
    return !value.empty() && value.size() <= 128 &&
           std::isalnum(static_cast<unsigned char>(value.front())) &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
             return std::isalnum(c) || c == '.' || c == '_' || c == '@' || c == ':' || c == '-';
           });
  };
  const auto safe_target =
      target_id.empty() || valid_identifier(target_id) ? target_id : std::string{};
  storage_->append_operator_audit_event({uuid(), operation_id, actor.id, actor.role, action,
                                         target_type, safe_target, outcome, http_status});
}
Json Service::operator_audit_page(std::size_t limit,
                                  std::optional<std::uint64_t> before_sequence) const {
  if (limit == 0 || limit > 100 || (before_sequence && *before_sequence == 0) ||
      (before_sequence &&
       *before_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
    throw Error(ErrorCode::Validation, "Invalid operator audit pagination");
  return storage_->operator_audit_page(limit, before_sequence);
}

void Service::sync_maintenance_registry(const std::string &state, const Actor &actor,
                                        const std::string &action, const std::string &from,
                                        const std::string &to) {
  if (!coordination_)
    return;
  try {
    coordination_->set_instance_state(state);
  } catch (...) {
    const auto now = timestamp();
    persist_maintenance_state(maintenance_mode_, maintenance_started_at_, actor.id,
                              {{"at", now},
                               {"actor", actor.id},
                               {"action", action},
                               {"from", from},
                               {"to", to},
                               {"instance_id", instance_id_},
                               {"result", "cluster_sync_failed"}});
    throw Error(ErrorCode::Storage, "Instance coordination state could not be updated");
  }
}
Json Service::operator_maintenance() const {
  MaintenanceMode mode;
  std::string since, actor;
  Json last_event = nullptr;
  {
    std::lock_guard lock(maintenance_mutex_);
    mode = maintenance_mode_;
    since = maintenance_started_at_;
    actor = maintenance_actor_;
    if (!operator_events_.empty())
      last_event = operator_events_.back();
  }
  const auto runtime = runtime_.diagnostics();
  const bool drained =
      mode == MaintenanceMode::Draining && runtime.active_runs == 0 && runtime.active_nodes == 0;
  const auto state = drained ? std::string("drained") : maintenance_mode_name(mode);
  bool cluster_visible = coordination_ == nullptr;
  std::string cluster_state = coordination_ ? "unknown" : "local";
  if (coordination_) {
    try {
      const auto current = coordination_->current_instance(config_.instance_stale_after_ms);
      if (current) {
        cluster_visible = current->state != "STALE";
        cluster_state = current->state;
      } else {
        cluster_visible = false;
        cluster_state = "missing";
      }
    } catch (const Error &) {
      cluster_visible = false;
      cluster_state = "unavailable";
    }
  }
  const bool cluster_quiescent = coordination_ == nullptr || cluster_state == "DRAINING" ||
                                 cluster_state == "DRAINED" || cluster_state == "MAINTENANCE";
  return {{"instance_id", instance_id_},
          {"state", state},
          {"desired_state", maintenance_mode_name(mode)},
          {"since", since.empty() ? Json(nullptr) : Json(since)},
          {"actor", actor.empty() ? Json(nullptr) : Json(actor)},
          {"cluster_state", cluster_state},
          {"cluster_visible", cluster_visible},
          {"owned_work", {{"runs", runtime.active_runs}, {"nodes", runtime.active_nodes}}},
          {"safe_to_stop", cluster_visible && cluster_quiescent && runtime.active_runs == 0 &&
                               runtime.active_nodes == 0 &&
                               (state == "drained" || state == "maintenance")},
          {"last_event", std::move(last_event)}};
}
Json Service::maintenance_metrics() const {
  MaintenanceMode mode;
  {
    std::lock_guard lock(maintenance_mutex_);
    mode = maintenance_mode_;
  }
  const auto runtime = runtime_.diagnostics();
  const bool drained =
      mode == MaintenanceMode::Draining && runtime.active_runs == 0 && runtime.active_nodes == 0;
  return {{"state", drained ? "drained" : maintenance_mode_name(mode)},
          {"owned_runs", runtime.active_runs},
          {"owned_nodes", runtime.active_nodes}};
}
void Service::request_drain(const Actor &actor) {
  require_operator_actor(actor);
  std::lock_guard lock(maintenance_mutex_);
  if (maintenance_mode_ == MaintenanceMode::Draining) {
    sync_maintenance_registry("DRAINING", actor, "drain", "draining", "draining");
    return;
  }
  if (maintenance_mode_ == MaintenanceMode::Maintenance) {
    sync_maintenance_registry("MAINTENANCE", actor, "drain", "maintenance", "maintenance");
    return;
  }
  const auto now = timestamp();
  runtime_.set_maintenance_mode(MaintenanceMode::Draining);
  try {
    persist_maintenance_state(MaintenanceMode::Draining, now, actor.id,
                              {{"at", now},
                               {"actor", actor.id},
                               {"action", "drain"},
                               {"from", "active"},
                               {"to", "draining"},
                               {"instance_id", instance_id_},
                               {"result", "requested"}});
  } catch (...) {
    runtime_.set_maintenance_mode(MaintenanceMode::Active);
    throw;
  }
  sync_maintenance_registry("DRAINING", actor, "drain", "active", "draining");
}
void Service::resume_instance(const Actor &actor) {
  require_operator_actor(actor);
  std::lock_guard lock(maintenance_mutex_);
  if (maintenance_mode_ == MaintenanceMode::Active)
    return;
  if (coordination_)
    sync_maintenance_registry("ACTIVE", actor, "resume", maintenance_mode_name(maintenance_mode_),
                              "active");
  const auto now = timestamp();
  persist_maintenance_state(MaintenanceMode::Active, "", actor.id,
                            {{"at", now},
                             {"actor", actor.id},
                             {"action", "resume"},
                             {"from", maintenance_mode_name(maintenance_mode_)},
                             {"to", "active"},
                             {"instance_id", instance_id_},
                             {"result", "requested"}});
  runtime_.set_maintenance_mode(MaintenanceMode::Active);
}
void Service::enter_maintenance(const Actor &actor) {
  require_operator_actor(actor);
  std::lock_guard lock(maintenance_mutex_);
  if (maintenance_mode_ == MaintenanceMode::Maintenance) {
    sync_maintenance_registry("MAINTENANCE", actor, "enter_maintenance", "maintenance",
                              "maintenance");
    return;
  }
  const auto runtime = runtime_.diagnostics();
  if (maintenance_mode_ != MaintenanceMode::Draining || runtime.active_runs != 0 ||
      runtime.active_nodes != 0)
    throw Error(ErrorCode::Conflict, "Instance must be drained before maintenance");
  if (coordination_) {
    const auto current = coordination_->current_instance(config_.instance_stale_after_ms);
    if (!current || current->state == "STALE")
      throw Error(ErrorCode::Storage, "Instance coordination state is unavailable");
  }
  const auto now = timestamp();
  runtime_.set_maintenance_mode(MaintenanceMode::Maintenance);
  try {
    persist_maintenance_state(MaintenanceMode::Maintenance, now, actor.id,
                              {{"at", now},
                               {"actor", actor.id},
                               {"action", "enter_maintenance"},
                               {"from", "draining"},
                               {"to", "maintenance"},
                               {"instance_id", instance_id_},
                               {"result", "requested"}});
  } catch (...) {
    runtime_.set_maintenance_mode(MaintenanceMode::Draining);
    throw;
  }
  sync_maintenance_registry("MAINTENANCE", actor, "enter_maintenance", "draining", "maintenance");
}
Service::~Service() noexcept {
  shutdown();
}
Json Service::register_pipeline(const std::string &yaml) {
  auto extensions = nodes_.names();
  auto p = parse_pipeline(yaml, {extensions.begin(), extensions.end()});
  if (!p.input_schema.empty())
    schemas_.validate_declaration(p.input_schema);
  if (!p.output_schema.empty())
    schemas_.validate_declaration(p.output_schema);
  for (const auto &[id, node] : p.nodes) {
    if (!node.input_schema.empty())
      schemas_.validate_declaration(node.input_schema);
    if (!node.output_schema.empty())
      schemas_.validate_declaration(node.output_schema);
    if (!node.schema.empty())
      schemas_.validate_declaration(node.schema);
  }

  const auto key = pipeline_reference(p.name, p.version);
  auto existing = Json();
  bool has_existing = false;
  try {
    existing = storage_->get(RecordKind::Pipeline, key);
    has_existing = true;
  } catch (const Error &error) {
    if (error.code != ErrorCode::NotFound)
      throw;
  }
  // Read records written by the pre-versioned registry as the v1 identity.
  if (!has_existing && p.version == 1) {
    try {
      existing = storage_->get(RecordKind::Pipeline, p.name);
      has_existing = true;
    } catch (const Error &error) {
      if (error.code != ErrorCode::NotFound)
        throw;
    }
  }
  if (has_existing) {
    if (!same_source(existing, yaml))
      throw Error(ErrorCode::Conflict, "Pipeline revision is immutable",
                  {{"pipeline", key}, {"reason", "definition differs"}});
    return existing;
  }

  const auto records = all_pipeline_records(*storage_);
  const auto candidate_key = key;
  auto revisions = [&](const std::string &name) {
    std::vector<Json> found;
    for (const auto &record : records)
      if (record.value("name", std::string{}) == name)
        found.push_back(record);
    if (p.name == name)
      found.push_back({{"id", candidate_key},
                       {"name", p.name},
                       {"version", p.version},
                       {"yaml", yaml},
                       {"resolved_subpipelines", Json::object()}});
    return found;
  };
  auto resolve_reference = [&](const std::string &reference) {
    const auto parsed = parse_pipeline_reference(reference);
    if (parsed.explicit_version) {
      const auto resolved = pipeline_reference(parsed.name, parsed.version);
      if (resolved == candidate_key)
        return resolved;
      for (const auto &record : records)
        if (record.value("id", std::string{}) == resolved ||
            (record.value("name", std::string{}) == parsed.name &&
             record.value("version", 1U) == parsed.version))
          return resolved;
      throw Error(ErrorCode::NotFound, "Referenced pipeline revision is not registered",
                  {{"pipeline", resolved}});
    }
    auto found = revisions(parsed.name);
    if (found.empty())
      throw Error(ErrorCode::NotFound, "Referenced pipeline is not registered",
                  {{"pipeline", parsed.name}});
    if (found.size() != 1)
      throw Error(ErrorCode::Conflict, "Unversioned pipeline reference is ambiguous",
                  {{"pipeline", parsed.name}, {"reason", "use name@version"}});
    return pipeline_reference(parsed.name, found.front().value("version", 1U));
  };
  for (const auto &[node_id, node] : p.nodes)
    if (node.type == "subpipeline")
      p.resolved_subpipelines[node_id] = resolve_reference(node.binding);

  std::set<std::string> active, visited;
  std::function<void(const std::string &)> visit = [&](const std::string &revision) {
    if (!active.insert(revision).second)
      throw Error(ErrorCode::Validation, "Recursive subpipeline dependency",
                  {{"pipeline", revision}});
    if (!visited.insert(revision).second) {
      active.erase(revision);
      return;
    }
    Json record;
    if (revision == candidate_key) {
      record = {{"yaml", yaml}, {"resolved_subpipelines", p.resolved_subpipelines}};
    } else {
      for (const auto &item : records)
        if (item.value("id", std::string{}) == revision ||
            (item.value("name", std::string{}) + "@" + std::to_string(item.value("version", 1U))) ==
                revision)
          record = item;
      if (record.is_null())
        throw Error(ErrorCode::NotFound, "Referenced pipeline revision is not registered",
                    {{"pipeline", revision}});
    }
    std::map<std::string, std::string> dependencies;
    if (record.contains("resolved_subpipelines")) {
      dependencies = record.at("resolved_subpipelines").get<std::map<std::string, std::string>>();
    } else {
      auto child = parse_pipeline(record.at("yaml").get<std::string>(),
                                  {extensions.begin(), extensions.end()});
      for (const auto &[node_id, node] : child.nodes)
        if (node.type == "subpipeline")
          dependencies[node_id] = resolve_reference(node.binding);
    }
    for (const auto &[node_id, child] : dependencies) {
      (void)node_id;
      visit(child);
    }
    active.erase(revision);
  };
  visit(candidate_key);

  Json record = {{"id", key},
                 {"name", p.name},
                 {"version", p.version},
                 {"laso", p.schema_version},
                 {"yaml", yaml},
                 {"definition_fingerprint", definition_fingerprint(yaml)},
                 {"resolved_subpipelines", p.resolved_subpipelines},
                 {"registered_at", timestamp()}};
  Event e;
  e.pipeline_id = p.name;
  e.metadata["pipeline_version"] = p.version;
  e.type = "pipeline.registered";
  try {
    storage_->commit(
        {{RecordKind::Pipeline, key, "", record}, {RecordKind::Event, e.id, "", Json(e)}});
  } catch (const Error &error) {
    // Another registration may have won the transaction between the
    // initial lookup and this commit. Preserve idempotence for the same
    // immutable source while still rejecting a conflicting revision.
    if (error.code != ErrorCode::Conflict)
      throw;
    try {
      const auto concurrent = storage_->get(RecordKind::Pipeline, key);
      if (same_source(concurrent, yaml))
        return concurrent;
    } catch (const Error &lookup_error) {
      if (lookup_error.code != ErrorCode::NotFound)
        throw;
    }
    throw;
  }
  events_.publish(e);
  return record;
}
bool Service::context_reduction_available() const {
  if (!config_.session_context_reduction_enabled)
    return false;
  try {
    (void)context_reducers_.get(config_.session_context_reducer);
    return true;
  } catch (const Error &error) {
    if (error.code == ErrorCode::NotFound)
      return false;
    throw;
  }
}
std::string Service::start(const std::string &name_or_path, const Json &input,
                           const std::string &actor, bool allow_file, Json origin,
                           Json message_metadata) {
  std::string name = name_or_path;
  if (allow_file && std::filesystem::is_regular_file(name_or_path))
    name = register_pipeline(read_document(name_or_path)).at("id").get<std::string>();
  auto extensions = nodes_.names();
  auto record = pipeline_record(name);
  auto p =
      parse_pipeline(record.at("yaml").get<std::string>(), {extensions.begin(), extensions.end()});
  if (record.contains("resolved_subpipelines"))
    p.resolved_subpipelines =
        record.at("resolved_subpipelines").get<std::map<std::string, std::string>>();
  return runtime_.run(p, input, actor, "", "", 0, "", std::move(origin),
                      std::move(message_metadata));
}

AgentSession Service::create_session(const std::string &pipeline_id) {
  auto admission = runtime_.admission_guard();
  if (!runtime_.accepts_new_work())
    throw Error(ErrorCode::Unavailable, "Instance is not accepting new work");
  (void)pipeline_record(pipeline_id);
  AgentSession session;
  session.pipeline_id = pipeline_id;
  storage_->commit({{RecordKind::AgentSession, session.id, session.id, Json(session)}});
  return session;
}
AgentSession Service::agent_session(const std::string &id) const {
  return storage_->get(RecordKind::AgentSession, id).get<AgentSession>();
}
void Service::close_session(const std::string &id) {
  Event event;
  event.run_id = id;
  event.type = "session.closed";
  storage_->close_agent_session(id, Json(event));
  const auto session = agent_session(id);
  if (session.state == "closing" && !session.active_run_id.empty()) {
    try {
      runtime_.cancel(session.active_run_id);
    } catch (const Error &error) {
      if (error.code != ErrorCode::Conflict && error.code != ErrorCode::NotFound)
        throw;
      const auto current = agent_session(id);
      if (current.state == "closing" && current.active_run_id == session.active_run_id)
        throw;
    }
  }
}
Json Service::submit_session_turn(const std::string &id, const std::string &idempotency_key,
                                  const Json &input) {
  auto admission = runtime_.admission_guard();
  if (!runtime_.accepts_new_work())
    throw Error(ErrorCode::Unavailable, "Instance is not accepting new work");
  if (idempotency_key.empty() || idempotency_key.size() > 512 || input.dump().size() > 1024 * 1024)
    throw Error(ErrorCode::Validation, "Invalid session input");
  const auto session = agent_session(id);
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto byte : idempotency_key) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  std::ostringstream turn_id;
  turn_id << id << "-turn-" << std::hex << hash;
  Json turn{{"idempotency_key", idempotency_key},
            {"input", input},
            {"state", "queued"},
            {"accepted_at", timestamp()},
            {"pipeline_id", session.pipeline_id}};
  Event event;
  event.run_id = id;
  event.type = "input.accepted";
  storage_->submit_session_turn(id, turn_id.str(), turn, Json(event));
  const auto accepted = storage_->get(RecordKind::SessionTurn, turn_id.str());
  try {
    runtime_.dispatch_session(id);
  } catch (...) {
    log_diagnostic("service.session_dispatch_deferred");
  }
  return accepted;
}
Json Service::create_session_context_generation(
    const std::string &id, std::uint64_t expected_generation, std::uint64_t through_turn_sequence,
    const std::string &idempotency_key, const std::string &representation_kind,
    const std::string &representation_version, const Json &payload) {
  (void)agent_session(id);
  return storage_->create_session_context_generation(id, expected_generation, through_turn_sequence,
                                                     idempotency_key, representation_kind,
                                                     representation_version, payload);
}
std::optional<Json> Service::latest_session_context_generation(const std::string &id) const {
  (void)agent_session(id);
  return storage_->latest_session_context_generation(id);
}
std::vector<Json> Service::session_events(const std::string &id, std::uint64_t after,
                                          std::size_t limit) const {
  (void)agent_session(id);
  return storage_->session_events(id, after, limit);
}

Json Service::create_schedule(const Json &spec) {
  auto schedule = parse_schedule_spec(spec);
  const auto raw_pipeline = schedule.pipeline_id;
  auto reference = parse_pipeline_reference(raw_pipeline);
  if (reference.explicit_version) {
    schedule.pipeline_id = reference.name;
    schedule.pipeline_version = reference.version;
  }
  const auto pipeline =
      reference.explicit_version
          ? pipeline_record(pipeline_reference(schedule.pipeline_id, schedule.pipeline_version))
          : pipeline_record(raw_pipeline);
  schedule.pipeline_id = pipeline.at("name").get<std::string>();
  schedule.pipeline_version = pipeline.at("version").get<unsigned>();
  if (schedule.next_due_at.empty()) {
    if (!schedule.at.empty())
      schedule.next_due_at = schedule.at;
    else
      schedule.next_due_at = next_schedule_due(schedule, timestamp());
  }
  validate_schedule(schedule);
  return scheduler_.create_schedule(std::move(schedule));
}

Json Service::update_schedule(const std::string &id, const Json &spec) {
  auto schedule = get(RecordKind::Schedule, id).get<ScheduleDefinition>();
  if (!spec.is_object())
    throw Error(ErrorCode::Validation, "Schedule update must be a JSON object");
  if (spec.contains("name"))
    schedule.name = spec.at("name").get<std::string>();
  if (spec.contains("type"))
    schedule.type = spec.at("type").get<std::string>();
  if (spec.contains("at"))
    schedule.at = spec.at("at").get<std::string>();
  if (spec.contains("cron"))
    schedule.cron = spec.at("cron").get<std::string>();
  if (spec.contains("interval_ms"))
    schedule.interval_ms = spec.at("interval_ms").is_string()
                               ? spec.at("interval_ms").get<std::string>()
                               : std::to_string(spec.at("interval_ms").get<std::int64_t>());
  if (spec.contains("input"))
    schedule.input = spec.at("input");
  if (spec.contains("misfire_policy"))
    schedule.misfire_policy = spec.at("misfire_policy").get<std::string>();
  if (spec.contains("overlap_policy"))
    schedule.overlap_policy = spec.at("overlap_policy").get<std::string>();
  if (spec.contains("enabled"))
    schedule.enabled = spec.at("enabled").get<bool>();
  if (spec.contains("pipeline") || spec.contains("pipeline_id") ||
      spec.contains("pipeline_version") || spec.contains("version")) {
    const auto raw = spec.value("pipeline", spec.value("pipeline_id", schedule.pipeline_id));
    const auto parsed = parse_pipeline_reference(raw);
    schedule.pipeline_id = parsed.explicit_version ? parsed.name : raw;
    if (parsed.explicit_version)
      schedule.pipeline_version = parsed.version;
    else if (spec.contains("pipeline_version"))
      schedule.pipeline_version = spec.at("pipeline_version").get<unsigned>();
    else if (spec.contains("version"))
      schedule.pipeline_version = spec.at("version").get<unsigned>();
  }
  if (spec.contains("next_due_at"))
    schedule.next_due_at = spec.at("next_due_at").get<std::string>();
  else if (spec.contains("at") || spec.contains("cron") || spec.contains("interval_ms"))
    schedule.next_due_at =
        schedule.type == "one_time" ? schedule.at : next_schedule_due(schedule, timestamp());
  const auto pipeline =
      pipeline_record(pipeline_reference(schedule.pipeline_id, schedule.pipeline_version));
  schedule.pipeline_id = pipeline.at("name").get<std::string>();
  schedule.pipeline_version = pipeline.at("version").get<unsigned>();
  return scheduler_.update_schedule(std::move(schedule));
}

void Service::set_schedule_enabled(const std::string &id, bool enabled) {
  scheduler_.set_schedule_enabled(id, enabled);
}
void Service::delete_schedule(const std::string &id) {
  scheduler_.delete_schedule(id);
}

Json Service::create_trigger(const Json &spec) {
  auto trigger = parse_trigger_spec(spec);
  const auto parsed = parse_pipeline_reference(trigger.pipeline_id);
  if (parsed.explicit_version) {
    trigger.pipeline_id = parsed.name;
    trigger.pipeline_version = parsed.version;
  }
  const auto pipeline =
      pipeline_record(pipeline_reference(trigger.pipeline_id, trigger.pipeline_version));
  trigger.pipeline_id = pipeline.at("name").get<std::string>();
  trigger.pipeline_version = pipeline.at("version").get<unsigned>();
  return scheduler_.create_trigger(std::move(trigger));
}

Json Service::update_trigger(const std::string &id, const Json &spec) {
  auto trigger = get(RecordKind::Trigger, id).get<TriggerDefinition>();
  if (!spec.is_object())
    throw Error(ErrorCode::Validation, "Trigger update must be a JSON object");
  if (spec.contains("name"))
    trigger.name = spec.at("name").get<std::string>();
  if (spec.contains("event") || spec.contains("event_type"))
    trigger.event_type = spec.value("event", spec.value("event_type", trigger.event_type));
  if (spec.contains("match"))
    trigger.match = spec.at("match");
  if (spec.contains("enabled"))
    trigger.enabled = spec.at("enabled").get<bool>();
  if (spec.contains("pipeline") || spec.contains("pipeline_id") ||
      spec.contains("pipeline_version") || spec.contains("version")) {
    const auto raw = spec.value("pipeline", spec.value("pipeline_id", trigger.pipeline_id));
    const auto parsed = parse_pipeline_reference(raw);
    trigger.pipeline_id = parsed.explicit_version ? parsed.name : raw;
    if (parsed.explicit_version)
      trigger.pipeline_version = parsed.version;
    else if (spec.contains("pipeline_version"))
      trigger.pipeline_version = spec.at("pipeline_version").get<unsigned>();
    else if (spec.contains("version"))
      trigger.pipeline_version = spec.at("version").get<unsigned>();
  }
  const auto pipeline =
      pipeline_record(pipeline_reference(trigger.pipeline_id, trigger.pipeline_version));
  trigger.pipeline_id = pipeline.at("name").get<std::string>();
  trigger.pipeline_version = pipeline.at("version").get<unsigned>();
  return scheduler_.update_trigger(std::move(trigger));
}

void Service::set_trigger_enabled(const std::string &id, bool enabled) {
  scheduler_.set_trigger_enabled(id, enabled);
}
void Service::delete_trigger(const std::string &id) {
  scheduler_.delete_trigger(id);
}
Json Service::event_sources() const {
  return plugins_.event_sources();
}
Json Service::event_source(const std::string &id) const {
  return plugins_.event_source(id);
}
void Service::set_event_source_enabled(const std::string &id, bool enabled) {
  plugins_.set_event_source_enabled(id, enabled);
}
Json Service::workers() const {
  Json result = Json::array();
  for (const auto &id : worker_registry_.names())
    result.push_back(worker_registry_.get(id)->metadata());
  return result;
}
Json Service::worker(const std::string &id) const {
  return Json(worker_registry_.get(id)->metadata());
}
std::vector<Json> Service::worker_jobs(const std::string &run_id, std::size_t limit,
                                       std::size_t offset) const {
  return worker_manager_->jobs(run_id, limit, offset);
}
Json Service::worker_job(const std::string &id) const {
  auto result = Json(worker_manager_->job(id));
  result.erase("_continuation_candidate");
  return result;
}
void Service::cancel_worker_job(const std::string &id) {
  worker_manager_->cancel(id, WorkerJobState::Cancelled, "Cancellation requested by operator");
}
std::vector<Json> Service::worker_interactions(const std::string &run_id, std::size_t limit,
                                               std::size_t offset) const {
  return worker_manager_->worker_interactions(run_id, limit, offset);
}
Json Service::worker_interaction(const std::string &id) const {
  return worker_manager_->worker_interaction(id);
}
void Service::resolve_worker_interaction(const std::string &id, WorkerInteractionState state,
                                         const Json &payload, const std::string &actor,
                                         const std::string &reason) {
  worker_manager_->resolve_interaction(id, state, payload, actor, reason);
}
Json Service::pipeline_record(const std::string &reference) const {
  const auto parsed = parse_pipeline_reference(reference);
  if (parsed.explicit_version) {
    const auto key = pipeline_reference(parsed.name, parsed.version);
    try {
      return storage_->get(RecordKind::Pipeline, key);
    } catch (const Error &error) {
      if (error.code != ErrorCode::NotFound)
        throw;
      if (parsed.version != 1)
        throw;
      // Compatibility for a database created before versioned registry keys.
      return storage_->get(RecordKind::Pipeline, parsed.name);
    }
  }
  try {
    return storage_->get(RecordKind::Pipeline, parsed.name);
  } catch (const Error &error) {
    if (error.code != ErrorCode::NotFound)
      throw;
  }
  Json found;
  for (const auto &record : all_pipeline_records(*storage_)) {
    if (record.value("name", std::string{}) != parsed.name)
      continue;
    if (!found.is_null())
      throw Error(ErrorCode::Conflict, "Unversioned pipeline reference is ambiguous",
                  {{"pipeline", parsed.name}, {"reason", "use name@version"}});
    found = record;
  }
  if (found.is_null())
    throw Error(ErrorCode::NotFound, "Pipeline is not registered", {{"pipeline", parsed.name}});
  return found;
}
PipelineDefinition Service::resolve_pipeline(const std::string &reference) const {
  const auto record = pipeline_record(reference);
  auto extensions = nodes_.names();
  auto p =
      parse_pipeline(record.at("yaml").get<std::string>(), {extensions.begin(), extensions.end()});
  if (record.contains("resolved_subpipelines"))
    p.resolved_subpipelines =
        record.at("resolved_subpipelines").get<std::map<std::string, std::string>>();
  return p;
}
Json Service::run_view(const std::string &id) const {
  auto result = storage_->get(RecordKind::Run, id);
  Json children = Json::array();
  for (std::size_t offset = 0;;) {
    auto page = storage_->list(RecordKind::Run, "", 10000, offset);
    for (const auto &item : page) {
      const auto child = item.get<Run>();
      if (child.parent_id == id)
        children.push_back({{"id", child.id},
                            {"pipeline_id", child.pipeline_id},
                            {"pipeline_version", child.pipeline_version},
                            {"parent_node_id", child.parent_node_id},
                            {"state", child.state}});
    }
    if (page.size() < 10000)
      break;
    offset += 10000;
  }
  result["children"] = std::move(children);
  return result;
}
namespace {
Json operator_run_summary(const Run &run) {
  return {{"id", run.id},
          {"pipeline_id", run.pipeline_id},
          {"pipeline_version", run.pipeline_version},
          {"state", run.state},
          {"created_at", run.created_at},
          {"updated_at", run.updated_at},
          {"active_node", run.active_node},
          {"error", run.error},
          {"cancellation_requested", run.cancellation_requested},
          {"owner_instance_id", run.owner_instance_id},
          {"fencing_token", run.fencing_token},
          {"lease_expires_at", run.lease_expires_at},
          {"worker", run.worker},
          {"worker_job_id", run.worker_job_id}};
}
Json operator_node_work_summary(const NodeWork &work) {
  return {{"id", work.id},
          {"run_id", work.run_id},
          {"node_id", work.node_id},
          {"group_id", work.group_id},
          {"index", work.index},
          {"state", work.state},
          {"created_at", work.created_at},
          {"updated_at", work.updated_at},
          {"attempt", work.attempt},
          {"attempt_id", work.attempt_id},
          {"owner_instance_id", work.owner_instance_id},
          {"fencing_token", work.fencing_token},
          {"claimed_at", work.claimed_at},
          {"last_renewed_at", work.last_renewed_at},
          {"lease_expires_at", work.lease_expires_at},
          {"required_worker_id", work.required_worker_id},
          {"required_capability", work.required_capability},
          {"error", work.error},
          {"result_present", work.result.has_value()}};
}
std::string safe_error_summary(std::string error);
Json operator_attempt_summary(const Json &value) {
  const auto attempt = value.get<NodeExecution>();
  Json summary = {{"id", attempt.id},
                  {"run_id", attempt.run_id},
                  {"node_id", attempt.node_id},
                  {"attempt", attempt.attempt},
                  {"state", attempt.state},
                  {"started_at", attempt.started_at},
                  {"finished_at", attempt.finished_at},
                  {"duration_ms", attempt.duration_ms},
                  {"worker_job_id", attempt.worker_job_id},
                  {"worker_id", attempt.worker_id}};
  if (!attempt.error.empty())
    summary["error_summary"] = safe_error_summary(attempt.error);
  return summary;
}
Json operator_worker_job_summary(const Json &value) {
  const auto job = value.get<WorkerJob>();
  Json usage = {{"provider", job.usage.provider},
                {"model", job.usage.model},
                {"executor", job.usage.executor}};
  if (job.usage.wall_duration_ms)
    usage["wall_duration_ms"] = *job.usage.wall_duration_ms;
  if (job.usage.total_tokens)
    usage["total_tokens"] = *job.usage.total_tokens;
  if (job.usage.cost_units)
    usage["cost_units"] = *job.usage.cost_units;
  return {{"id", job.id},
          {"run_id", job.run_id},
          {"node_id", job.node_id},
          {"worker_id", job.worker_id},
          {"attempt", job.attempt},
          {"state", job.state},
          {"failure_kind", job.failure_kind},
          {"submitted_at", job.submitted_at},
          {"started_at", job.started_at},
          {"completed_at", job.completed_at},
          {"external_job_id", job.external_job_id},
          {"error", job.error},
          {"cancellation_error", job.cancellation_error},
          {"cancellation_requested", job.cancellation_requested},
          {"cancellation_acknowledged", job.cancellation_acknowledged},
          {"result_present", !job.result.is_null() && !job.result.empty()},
          {"artifact_count", job.artifacts.size()},
          {"usage", std::move(usage)}};
}
Json operator_artifact_summary(const Json &value) {
  const auto artifact = value.get<Artifact>();
  Json result = {{"id", artifact.id},
                 {"run_id", artifact.run_id},
                 {"node_id", artifact.node_id},
                 {"name_present", !artifact.name.empty()},
                 {"media_type", artifact.media_type},
                 {"created_at", artifact.created_at},
                 {"sha256", artifact.sha256},
                 {"size", artifact.size},
                 {"location_present", !artifact.location.empty()}};
  // Only expose integrity/provenance keys; arbitrary metadata may contain
  // paths or provider-specific content and is deliberately not dumped.
  for (const auto *key : {"sha256", "size", "attempt_id", "worker_id", "fencing_token"})
    if (artifact.metadata.contains(key))
      result["metadata"][key] = artifact.metadata.at(key);
  return result;
}
std::string safe_error_summary(std::string error) {
  std::transform(error.begin(), error.end(), error.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (error.find("timeout") != std::string::npos || error.find("timed out") != std::string::npos)
    return "Operation timed out";
  if (error.find("cancel") != std::string::npos)
    return "Operation was cancelled";
  if (error.find("capacity") != std::string::npos || error.find("limit") != std::string::npos)
    return "Capacity limit was reached";
  if (error.find("policy") != std::string::npos || error.find("denied") != std::string::npos ||
      error.find("permission") != std::string::npos)
    return "Policy or permission check failed";
  if (error.find("postgres") != std::string::npos || error.find("database") != std::string::npos ||
      error.find("storage") != std::string::npos)
    return "Storage operation failed";
  if (error.find("provider") != std::string::npos || error.find("worker") != std::string::npos)
    return "Provider or worker operation failed";
  return "Execution failed; details are withheld";
}
void redact_error(Json &value, const char *field, const char *summary_field) {
  if (!value.contains(field) || !value.at(field).is_string()) {
    value.erase(field);
    return;
  }
  const auto error = value.at(field).get<std::string>();
  value.erase(field);
  if (!error.empty())
    value[summary_field] = safe_error_summary(error);
}
std::string safe_worker_request_type(const Json &value) {
  const auto type = value.value("request_type", std::string{});
  if (type == "approval" || type == "permission" || type == "question")
    return type;
  return "other";
}
std::string safe_worker_request_state(const Json &value) {
  const auto state = value.value("state", std::string{});
  if (state == "pending" || state == "approved" || state == "denied" || state == "answered" ||
      state == "cancelled" || state == "expired")
    return state;
  return "unknown";
}
} // namespace
std::vector<Json> Service::inspect_runs() const {
  std::vector<Json> result;
  for (const auto &value : storage_->list(RecordKind::Run, "", 10000, 0))
    result.push_back(operator_run_summary(value.get<Run>()));
  return result;
}
Json Service::inspect_run(const std::string &id) const {
  const auto run = storage_->get(RecordKind::Run, id).get<Run>();
  Json result = operator_run_summary(run);
  result["children"] = Json::array();
  result["node_work"] = Json::array();
  result["attempts"] = Json::array();
  result["worker_jobs"] = Json::array();
  result["artifacts"] = Json::array();
  for (const auto &value : storage_->list(RecordKind::Run, "", 10000, 0)) {
    const auto child = value.get<Run>();
    if (child.parent_id == id)
      result["children"].push_back(operator_run_summary(child));
  }
  for (const auto &value : storage_->list(RecordKind::NodeWork, id, 10000, 0))
    result["node_work"].push_back(operator_node_work_summary(value.get<NodeWork>()));
  for (const auto &value : storage_->list(RecordKind::Attempt, id, 10000, 0))
    result["attempts"].push_back(operator_attempt_summary(value));
  for (const auto &value : worker_manager_->jobs(id, 10000, 0))
    result["worker_jobs"].push_back(operator_worker_job_summary(value));
  for (const auto &value : storage_->list(RecordKind::Artifact, id, 10000, 0))
    result["artifacts"].push_back(operator_artifact_summary(value));
  return result;
}
std::vector<Json> Service::inspect_node_works(const std::string &run_id) const {
  std::vector<Json> result;
  for (const auto &value : storage_->list(RecordKind::NodeWork, run_id, 10000, 0))
    result.push_back(operator_node_work_summary(value.get<NodeWork>()));
  return result;
}
Json Service::inspect_node_work(const std::string &id) const {
  return operator_node_work_summary(storage_->get(RecordKind::NodeWork, id).get<NodeWork>());
}
std::vector<Json> Service::inspect_artifacts(const std::string &run_id) const {
  std::vector<Json> result;
  for (const auto &value : storage_->list(RecordKind::Artifact, run_id, 10000, 0))
    result.push_back(operator_artifact_summary(value));
  return result;
}
Json Service::artifact_integrity() const {
  const auto report = artifacts_->integrity();
  Json errors = Json::array();
  for (const auto &error : report.errors)
    errors.push_back(error);
  return {{"root", artifacts_->root().filename().string()},
          {"objects", report.objects},
          {"verified", report.verified},
          {"invalid", report.invalid},
          {"temporary", report.temporary},
          {"errors", std::move(errors)}};
}
Json Service::artifact_gc(bool dry_run, std::uint64_t grace_seconds) {
  return artifacts_->collect_garbage(dry_run, grace_seconds);
}
Json Service::operator_status() const {
  const auto storage = storage_->operator_diagnostics();
  const auto runtime = runtime_.diagnostics();
  Json coordination = {{"enabled", coordination_ != nullptr}};
  if (coordination_) {
    const auto diagnostics = coordination_->diagnostics();
    coordination.update({{"pool_size", diagnostics.pool_size},
                         {"pool_in_use", diagnostics.pool_in_use},
                         {"pool_acquisition_timeouts", diagnostics.pool_acquisition_timeouts},
                         {"pool_replacements", diagnostics.pool_replacements},
                         {"lease_acquisition_failures", diagnostics.lease_acquisition_failures},
                         {"renewal_failures", diagnostics.renewal_failures},
                         {"fencing_rejections", diagnostics.fencing_rejections}});
  }
  return {{"system",
           {{"version", version}, {"api_version", 1}, {"pipeline_schema", 1}, {"plugin_abi", 1}}},
          {"storage", storage},
          {"execution",
           {{"mode", config_.execution_mode},
            {"instance_id", instance_id_},
            {"state", runtime.stopping ? "stopping" : operator_maintenance().at("state")}}},
          {"maintenance", operator_maintenance()},
          {"coordination", std::move(coordination)},
          {"capacity",
           {{"active_runs", runtime.active_runs},
            {"active_nodes", runtime.active_nodes},
            {"limits",
             {{"active_runs", config_.max_runs},
              {"pending_runs", config_.max_pending_runs},
              {"active_nodes", config_.max_nodes},
              {"nodes_per_run", config_.max_nodes_per_run},
              {"worker_jobs", config_.max_worker_jobs},
              {"worker_jobs_per_worker", config_.max_worker_jobs_per_worker},
              {"session_sse_streams", config_.max_session_sse_streams},
              {"postgres_pool_connections", config_.postgres_pool_max_connections}}}}},
          {"queues",
           {{"runs_by_state", storage.value("run_states", Json::object())},
            {"worker_jobs_by_state", storage.value("worker_job_states", Json::object())}}},
          {"recent_failures", storage.value("recent_failures", Json::array())},
          {"recovery_contract",
           {{"provider_attempts", "at-least-once"},
            {"explicit_resume_may_replay_unfinished_node", true},
            {"database_rollback", "restore-compatible-backup"}}}};
}

Json Service::readiness() const {
  const auto runtime = runtime_.diagnostics();
  bool storage_available = false;
  std::string schema_state = "unknown";
  try {
    const auto storage = storage_->readiness_diagnostics();
    storage_available = storage.value("available", false);
    const auto reported_schema = storage.value("schema_state", std::string{"unknown"});
    schema_state = reported_schema == "current"    ? "current"
                   : reported_schema == "mismatch" ? "incompatible"
                                                   : "unknown";
  } catch (const Error &error) {
    if (error.code != ErrorCode::Storage && error.code != ErrorCode::Capacity)
      throw;
  }

  // Readiness must stay on the bounded storage probe path. Operator status also
  // reads the coordination registry, which is unnecessary for local admission
  // and could add an unbounded second database wait during pool/database failure.
  const auto maintenance = maintenance_metrics();
  const auto runtime_state =
      runtime.stopping ? "stopping" : maintenance.at("state").get<std::string>();
  const bool ready = storage_available && schema_state == "current" && !runtime.stopping &&
                     runtime_state == "active";
  return {{"status", ready ? "ready" : "not_ready"},
          {"checks",
           {{"storage", storage_available ? "available" : "unavailable"},
            {"schema", schema_state},
            {"runtime", runtime_state},
            {"maintenance", runtime_state == "active" ? "accepting" : "not_accepting"}}}};
}

Json Service::operator_artifact_integrity() const {
  try {
    const auto report = artifacts_->integrity_bounded(10, 16 * 1024 * 1024);
    const auto has_errors = !report.errors.empty();
    return {{"supported", true},
            {"state", report.invalid || has_errors ? "degraded"
                      : report.complete            ? "verified"
                                                   : "partial"},
            {"objects", report.objects},
            {"verified", report.verified},
            {"invalid", report.invalid},
            {"unverified", report.unverified},
            {"entries_scanned", report.entries_scanned},
            {"complete", report.complete},
            {"truncated", !report.complete},
            {"verification", report.verification},
            {"temporary", report.temporary},
            {"errors_present", has_errors}};
  } catch (const Error &error) {
    if (error.code != ErrorCode::Policy)
      throw;
    return {{"supported", false}, {"state", "not_supported"}};
  }
}

Json Service::operator_page(const std::string &resource, std::size_t limit, std::size_t offset,
                            const std::string &parent_id) const {
  if (limit == 0 || limit > 100 || offset > 100000000)
    throw Error(ErrorCode::Validation, "Operator pagination limit exceeded");
  Json result = Json::array();
  if (resource == "runs") {
    for (const auto &value : storage_->list(RecordKind::Run, "", limit, offset)) {
      auto summary = operator_run_summary(value.get<Run>());
      redact_error(summary, "error", "error_summary");
      result.push_back(std::move(summary));
    }
  } else if (resource == "node-work") {
    for (const auto &value : storage_->list(RecordKind::NodeWork, "", limit, offset)) {
      auto summary = operator_node_work_summary(value.get<NodeWork>());
      redact_error(summary, "error", "error_summary");
      result.push_back(std::move(summary));
    }
  } else if (resource == "worker-jobs") {
    for (const auto &value : worker_manager_->jobs("", limit, offset)) {
      auto summary = operator_worker_job_summary(value);
      summary.erase("external_job_id");
      redact_error(summary, "error", "error_summary");
      redact_error(summary, "cancellation_error", "cancellation_error_summary");
      for (const auto *key : {"provider", "model", "executor"})
        summary["usage"].erase(key);
      result.push_back(std::move(summary));
    }
  } else if (resource == "attempts") {
    for (const auto &value : storage_->list(RecordKind::Attempt, "", limit, offset))
      result.push_back(operator_attempt_summary(value));
  } else if (resource == "artifacts") {
    for (const auto &value : storage_->list(RecordKind::Artifact, "", limit, offset))
      result.push_back(operator_artifact_summary(value));
  } else if (resource == "sessions") {
    for (const auto &value : storage_->list(RecordKind::AgentSession, "", limit, offset)) {
      const auto session = value.get<AgentSession>();
      result.push_back({{"id", session.id},
                        {"pipeline_id", session.pipeline_id},
                        {"state", session.state},
                        {"created_at", session.created_at},
                        {"updated_at", session.updated_at},
                        {"active_turn_id", session.active_turn_id},
                        {"active_run_id", session.active_run_id},
                        {"next_sequence", session.next_sequence}});
    }
  } else if (resource == "session-turns") {
    if (parent_id.empty())
      throw Error(ErrorCode::Validation, "Session id is required");
    for (const auto &value : storage_->list(RecordKind::SessionTurn, parent_id, limit, offset)) {
      Json summary = {{"id", value.value("id", std::string{})},
                      {"session_id", value.value("session_id", std::string{})},
                      {"sequence", value.value("sequence", std::uint64_t{0})},
                      {"state", value.value("state", std::string{})},
                      {"accepted_at", value.value("accepted_at", std::string{})},
                      {"run_id", value.value("run_id", std::string{})},
                      {"input_present", value.contains("input")}};
      result.push_back(std::move(summary));
    }
  } else if (resource == "approvals") {
    for (const auto &value : storage_->list(RecordKind::Approval, "", limit, offset)) {
      const auto approval = value.get<Approval>();
      result.push_back({{"id", approval.id},
                        {"run_id", approval.run_id},
                        {"node_id", approval.node_id},
                        {"action", approval.action},
                        {"visit", approval.visit},
                        {"decision", approval.decision},
                        {"created_at", approval.created_at},
                        {"decided_at", approval.decided_at},
                        {"actor", approval.actor}});
    }
  } else if (resource == "worker-requests") {
    for (const auto &value : worker_manager_->worker_interactions("", limit, offset)) {
      result.push_back({{"id", value.value("id", std::string{})},
                        {"worker_job_id", value.value("worker_job_id", std::string{})},
                        {"worker_id", value.value("worker_id", std::string{})},
                        {"run_id", value.value("run_id", std::string{})},
                        {"session_id", value.value("session_id", std::string{})},
                        {"request_type", safe_worker_request_type(value)},
                        {"state", safe_worker_request_state(value)},
                        {"created_at", value.value("created_at", std::string{})},
                        {"deadline", value.value("deadline", std::string{})},
                        {"details_withheld", true},
                        {"actor", value.value("actor", std::string{})},
                        {"decided_at", value.value("decided_at", std::string{})}});
    }
  } else if (resource == "providers") {
    const auto names = providers_.names();
    const auto end = std::min(names.size(), offset + limit);
    for (auto index = offset; index < end; ++index) {
      const auto &name = names[index];
      const auto provider = providers_.get(name);
      const auto metadata = provider->metadata();
      result.push_back({{"name", name},
                        {"version", metadata.version},
                        {"remote", metadata.remote},
                        {"streaming", metadata.streaming},
                        {"context_size", metadata.context_size},
                        {"continuation_mode", continuation_mode_name(metadata.continuation_mode)},
                        {"healthy", provider->health().healthy},
                        {"capability_count", metadata.capabilities.size()}});
    }
  } else if (resource == "plugins") {
    const auto &plugin_infos = plugins_.plugins();
    const auto end = std::min(plugin_infos.size(), offset + limit);
    for (auto index = offset; index < end; ++index) {
      const auto &plugin = plugin_infos[index];
      Json summary = {{"name", plugin.name},
                      {"version", plugin.version},
                      {"abi", plugin.abi},
                      {"loaded", plugin.loaded}};
      if (!plugin.error.empty())
        summary["error_summary"] = "Plugin load failed; deployment details are withheld";
      result.push_back(std::move(summary));
    }
  } else if (resource == "workers") {
    const auto names = worker_registry_.names();
    const auto end = std::min(names.size(), offset + limit);
    for (auto index = offset; index < end; ++index) {
      const auto metadata = worker_registry_.get(names[index])->metadata();
      result.push_back({{"id", metadata.id},
                        {"version", metadata.version},
                        {"status", metadata.status},
                        {"local", metadata.local},
                        {"remote", metadata.remote},
                        {"healthy", metadata.healthy},
                        {"enabled", metadata.enabled},
                        {"supports_recovery", metadata.supports_recovery},
                        {"supports_cancellation", metadata.supports_cancellation},
                        {"capability_count", metadata.capabilities.size()}});
    }
  } else if (resource == "instances") {
    if (coordination_)
      for (const auto &instance :
           coordination_->list_instances(config_.instance_stale_after_ms, limit, offset))
        result.push_back({{"instance_id", instance.instance_id},
                          {"started_at", instance.started_at},
                          {"last_heartbeat_at", instance.last_heartbeat_at},
                          {"software_version", instance.software_version},
                          {"state", instance.state}});
  } else if (resource == "leases") {
    if (coordination_)
      for (const auto &lease : coordination_->list_leases(limit, offset))
        result.push_back({{"resource_key", lease.resource_key},
                          {"owner_instance", lease.owner_instance},
                          {"fencing_token", lease.fencing_token},
                          {"acquired_at", lease.acquired_at},
                          {"heartbeat_at", lease.heartbeat_at},
                          {"expires_at", lease.expires_at},
                          {"active", lease.active}});
  } else {
    throw Error(ErrorCode::NotFound, "Operator resource is not available");
  }
  return result;
}

std::vector<Json> Service::inspect_worker_jobs(const std::string &run_id) const {
  std::vector<Json> result;
  for (const auto &value : worker_manager_->jobs(run_id, 10000, 0))
    result.push_back(operator_worker_job_summary(value));
  return result;
}
Json Service::inspect_worker_job(const std::string &id) const {
  return operator_worker_job_summary(Json(worker_manager_->job(id)));
}
Json Service::providers() const {
  Json result = Json::array();
  for (const auto &name : providers_.names()) {
    auto p = providers_.get(name);
    auto m = p->metadata();
    result.push_back({{"name", name},
                      {"version", m.version},
                      {"remote", m.remote},
                      {"streaming", m.streaming},
                      {"context_size", m.context_size},
                      {"plugin", m.plugin},
                      {"continuation_mode", continuation_mode_name(m.continuation_mode)},
                      {"healthy", p->health().healthy},
                      {"capabilities", m.capabilities}});
  }
  return result;
}
Json Service::tools() const {
  Json result = Json::array();
  for (const auto &name : tools_.names()) {
    auto m = tools_.get(name)->metadata();
    result.push_back({{"name", name},
                      {"description", m.description},
                      {"permission", m.permission},
                      {"network", m.network},
                      {"approval_required", m.approval_required},
                      {"plugin", m.plugin},
                      {"timeout_ms", m.timeout.count()},
                      {"input_schema", m.input_schema},
                      {"output_schema", m.output_schema}});
  }
  return result;
}
Json Service::plugins() const {
  return Json(plugins_.plugins());
}
void Service::shutdown() {
  if (shutdown_)
    return;
  shutdown_ = true;
  ingress_.stop();
  plugins_.stop_event_sources();
  worker_manager_->stop();
  scheduler_.stop();
  runtime_.shutdown();
  plugins_.stop_workers();
  for (auto it = process_workers_.rbegin(); it != process_workers_.rend(); ++it)
    (*it)->stop();
  if (coordination_) {
    try {
      coordination_->set_instance_state("STOPPED");
    } catch (const Error &) {
    }
  }
}
Json Service::instances() const {
  if (!coordination_)
    return Json::array();
  constexpr std::size_t page_size = 100;
  Json result = Json::array();
  for (std::size_t offset = 0;; offset += page_size) {
    const auto page =
        coordination_->list_instances(config_.instance_stale_after_ms, page_size, offset);
    for (const auto &instance : page)
      result.push_back(instance);
    if (page.size() < page_size)
      break;
  }
  return result;
}
void Service::recover_history() {
  if (config_.execution_mode == "multi_instance")
    return;
  for (std::size_t offset = 0;; offset += 1000) {
    auto records = storage_->list(RecordKind::Run, "", 1000, offset);
    for (const auto &record : records) {
      auto r = record.get<Run>();
      // A terminal checkpoint is immutable.  A stale cancellation flag must not
      // turn a completed run into a different terminal state during recovery.
      if (terminal(r.state))
        continue;
      if (r.cancellation_requested) {
        r.state = RunState::Cancelled;
        r.updated_at = timestamp();
        Event event;
        event.run_id = r.id;
        event.pipeline_id = r.pipeline_id;
        event.node_id = r.active_node;
        event.type = "run.cancelled";
        storage_->commit({{RecordKind::Run, r.id, r.id, Json(r)},
                          {RecordKind::Event, event.id, r.id, Json(event)}});
        continue;
      }
      if (r.state == RunState::WaitingApproval || r.state == RunState::Paused ||
          r.state == RunState::Queued)
        continue;
      r.state = RunState::Paused;
      r.error = "Process stopped during execution; explicit resume may replay the unfinished node";
      r.updated_at = timestamp();
      Event event;
      event.run_id = r.id;
      event.pipeline_id = r.pipeline_id;
      event.node_id = r.active_node;
      event.type = "run.recovery_required";
      std::vector<Record> checkpoint{{RecordKind::Run, r.id, r.id, Json(r)},
                                     {RecordKind::Event, event.id, r.id, Json(event)}};
      for (std::size_t attempt_offset = 0;;) {
        constexpr std::size_t page_size = 10000;
        auto attempts = storage_->list(RecordKind::Attempt, r.id, page_size, attempt_offset);
        for (const auto &item : attempts) {
          auto a = item.get<NodeExecution>();
          if (a.state == NodeState::Running) {
            a.state = NodeState::Failed;
            a.error = "Interrupted by process termination";
            a.finished_at = timestamp();
            checkpoint.push_back({RecordKind::Attempt, a.id, r.id, Json(a)});
          }
        }
        if (attempts.size() < page_size)
          break;
        attempt_offset += page_size;
      }
      storage_->commit(checkpoint);
    }
    if (records.size() < 1000)
      break;
  }
}
} // namespace laso
