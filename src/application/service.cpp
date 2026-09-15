#include <laso/application/service.hpp>
#include <laso/pipeline/parser.hpp>

namespace laso {
namespace {
Config checked(Config c) {
  c.validate();
  return c;
}
} // namespace
Service::Service(asio::io_context &io, Config config)
    : config_(checked(std::move(config))), lease_(config_.db_path), storage_(config_.db_path),
      policy_(config_.rules, config_.allow_network), schemas_(config_.schema_roots),
      plugins_(tools_, providers_),
      runtime_(io, config_,
               {storage_, events_, providers_, tools_, functions_, nodes_, policy_, schemas_}),
      artifacts_(config_.data_dir / "artifacts", storage_),
      scheduler_(
          io, [this](const ScheduledPipeline &s) { start(s.pipeline_id, s.input, "scheduler"); }) {
  configure_logging(config_);
  providers_.add("mock", std::make_shared<MockModelProvider>());
  if (!config_.local_openai_endpoint.empty())
    providers_.add("local-openai",
                   std::make_shared<LocalOpenAICompatibleProvider>(config_.local_openai_endpoint));
  tools_.add("echo", std::make_shared<EchoTool>());
  register_functions(functions_);
  plugins_.discover(config_.plugin_dirs);
  recover_history();
}
Json Service::register_pipeline(const std::string &yaml) {
  auto extensions = nodes_.names();
  auto p = parse_pipeline(yaml, {extensions.begin(), extensions.end()});
  for (const auto &[id, node] : p.nodes) {
    if (!node.input_schema.empty())
      schemas_.validate_declaration(node.input_schema);
    if (!node.output_schema.empty())
      schemas_.validate_declaration(node.output_schema);
    if (!node.schema.empty())
      schemas_.validate_declaration(node.schema);
  }
  Json record = {{"id", p.name},         {"name", p.name},
                 {"version", p.version}, {"laso", p.schema_version},
                 {"yaml", yaml},         {"registered_at", timestamp()}};
  Event e;
  e.pipeline_id = p.name;
  e.type = "pipeline.registered";
  storage_.commit(
      {{RecordKind::Pipeline, p.name, "", record}, {RecordKind::Event, e.id, "", Json(e)}});
  events_.publish(e);
  return record;
}
std::string Service::start(const std::string &name_or_path, const Json &input,
                           const std::string &actor, bool allow_file) {
  std::string name = name_or_path;
  if (allow_file && std::filesystem::is_regular_file(name_or_path))
    name = register_pipeline(read_document(name_or_path)).at("name").get<std::string>();
  auto extensions = nodes_.names();
  const auto p =
      parse_pipeline(storage_.get(RecordKind::Pipeline, name).at("yaml").get<std::string>(),
                     {extensions.begin(), extensions.end()});
  return runtime_.run(p, input, actor);
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
  scheduler_.stop();
  runtime_.shutdown();
}
void Service::recover_history() {
  for (std::size_t offset = 0;; offset += 1000) {
    auto records = storage_.list(RecordKind::Run, "", 1000, offset);
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
        storage_.commit({{RecordKind::Run, r.id, r.id, Json(r)},
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
        auto attempts = storage_.list(RecordKind::Attempt, r.id, page_size, attempt_offset);
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
      storage_.commit(checkpoint);
    }
    if (records.size() < 1000)
      break;
  }
}
} // namespace laso
