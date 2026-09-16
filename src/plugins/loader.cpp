#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <laso/core/config.hpp>
#include <laso/plugins/loader.hpp>
#include <laso_plugin.h>
#include <mutex>
#include <regex>
#include <set>

namespace laso {
std::string bounded(const char *value, std::size_t maximum) {
  if (!value)
    throw Error(ErrorCode::Plugin, "Null plugin metadata");
  auto length = strnlen(value, maximum + 1);
  if (length > maximum)
    throw Error(ErrorCode::Plugin, "Plugin metadata exceeds limit");
  return {value, length};
}
struct Library {
  void *library = nullptr;
  laso_plugin_handle handle = nullptr;
  void (*shutdown)(laso_plugin_handle) = nullptr;
  ~Library() {
    if (shutdown && handle) {
      try {
        shutdown(handle);
        // NOLINTNEXTLINE(bugprone-empty-catch): plugin shutdown cannot throw from this RAII path.
      } catch (
          ...) { // NOLINT(bugprone-empty-catch): shutdown must not throw from the RAII destructor.
        // A plugin must not throw across the C ABI; destruction is noexcept even
        // when a non-conforming plugin violates that contract.
      }
    }
    if (library)
      dlclose(library);
  }
};
struct ToolRegistration {
  ToolMetadata metadata;
  void *instance;
  laso_invoke_fn invoke;
};
struct ProviderRegistration {
  ProviderMetadata metadata;
  void *instance;
  laso_invoke_fn invoke;
  laso_health_fn health;
};
struct EventRegistration {
  std::string name, event_schema;
  std::vector<std::string> capabilities;
  void *instance = nullptr;
  laso_health_fn health = nullptr;
  laso_event_start_fn start = nullptr;
  laso_event_stop_fn stop = nullptr;
};
struct Staging {
  std::vector<ToolRegistration> tools;
  std::vector<ProviderRegistration> providers;
  std::vector<EventRegistration> events;
  bool failed = false;
};
int32_t register_component(void *opaque, const laso_component *c) noexcept {
  auto &staging = *static_cast<Staging *>(opaque);
  try {
    constexpr auto required_size = offsetof(laso_component, health);
    if (!c || c->struct_size < required_size)
      throw Error(ErrorCode::Plugin, "Invalid component");
    if (c->kind != LASO_COMPONENT_TOOL && c->kind != LASO_COMPONENT_MODEL &&
        c->kind != LASO_COMPONENT_EVENT) {
      staging.failed = true;
      return LASO_UNSUPPORTED;
    }
    if (staging.tools.size() + staging.providers.size() + staging.events.size() >= 64)
      throw Error(ErrorCode::Plugin, "Too many plugin registrations");
    const auto name = bounded(c->name, 128);
    if (!std::regex_match(name, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")))
      throw Error(ErrorCode::Plugin, "Invalid component name");
    auto json = Json::parse(bounded(c->metadata_json, 16384));
    if (!json.is_object())
      throw Error(ErrorCode::Plugin, "Component metadata must be an object");
    if (c->kind == LASO_COMPONENT_EVENT) {
      constexpr auto event_size = offsetof(laso_component, event_stop) + sizeof(laso_event_stop_fn);
      if (c->struct_size < event_size || !c->event_start || !c->event_stop)
        throw Error(ErrorCode::Plugin, "Invalid event source component");
      const auto schema = json.value("event_schema", std::string{});
      if (schema.size() > 512)
        throw Error(ErrorCode::Plugin, "Event source schema reference is too long");
      const auto capabilities = json.value("capabilities", std::vector<std::string>{});
      if (capabilities.size() > 32 ||
          std::any_of(capabilities.begin(), capabilities.end(),
                      [](const auto &value) { return value.empty() || value.size() > 64; }))
        throw Error(ErrorCode::Plugin, "Too many event source capabilities");
      staging.events.push_back({name, schema, capabilities, c->instance,
                                c->struct_size >= sizeof(laso_component) ? c->health : nullptr,
                                c->event_start, c->event_stop});
      return LASO_OK;
    }
    if (!c->invoke)
      throw Error(ErrorCode::Plugin, "Invalid component");
    auto timeout = json.value("timeout_ms", 30000);
    if (timeout < 1 || timeout > 3600000)
      throw Error(ErrorCode::Plugin, "Invalid component timeout");
    if (c->kind == LASO_COMPONENT_TOOL) {
      ToolMetadata m;
      m.name = name;
      m.description = json.value("description", std::string{});
      m.network = json.value("network", false);
      m.approval_required = json.value("approval_required", false);
      m.permission = json.value("permission", std::string("local"));
      m.input_schema = json.value("input_schema", Json::object());
      m.output_schema = json.value("output_schema", Json::object());
      m.timeout = Milliseconds(timeout);
      staging.tools.push_back({std::move(m), c->instance, c->invoke});
    } else {
      ProviderMetadata m;
      m.name = name;
      m.version = json.value("version", std::string("1"));
      m.remote = json.value("remote", false);
      m.network = json.value("network", false);
      m.streaming = json.value("streaming", false);
      m.context_size = json.value("context_size", std::size_t{4096});
      m.timeout = Milliseconds(timeout);
      m.capabilities = json.value("capabilities", std::vector<std::string>{"chat-completions"});
      if (m.context_size == 0 || m.context_size > 100000000 || m.capabilities.size() > 32)
        throw Error(ErrorCode::Plugin, "Invalid provider metadata");
      const auto health = c->struct_size >= sizeof(laso_component) ? c->health : nullptr;
      staging.providers.push_back({std::move(m), c->instance, c->invoke, health});
    }
    return LASO_OK;
  } catch (...) {
    staging.failed = true;
    return LASO_INVALID;
  }
}
struct Call {
  ExecutionContext &context;
  std::string output;
  bool written = false, failed = false;
};
int32_t call_should_stop(void *opaque) noexcept {
  auto &call = *static_cast<Call *>(opaque);
  return call.context.stop.stop_requested() ||
         std::chrono::steady_clock::now() >= call.context.deadline;
}
int32_t write_json(void *opaque, const char *bytes, uint64_t length) noexcept {
  auto &call = *static_cast<Call *>(opaque);
  try {
    if (!bytes || length > std::uint64_t{1024} * 1024 || call.written) {
      call.failed = true;
      return LASO_BUFFER_LIMIT;
    }
    call.output.assign(bytes, static_cast<std::size_t>(length));
    call.written = true;
    return LASO_OK;
  } catch (...) {
    call.failed = true;
    return LASO_FAILED;
  }
}
class PluginTool final : public Tool {
public:
  PluginTool(std::shared_ptr<Library> library, ToolRegistration registration)
      : library_(std::move(library)), registration_(std::move(registration)) {}
  ToolMetadata metadata() const override {
    return registration_.metadata;
  }
  Task<ToolResult> invoke(const ToolRequest &r, ToolContext &c) override {
    try {
      c.execution.check();
      // ABI v1 callbacks are short, cooperative local calls, serialized per component.
      // Remote/nonblocking provider adapters belong in a future async ABI revision.
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock.owns_lock())
        throw Error(ErrorCode::Tool, "Plugin component is busy");
      Call call{c.execution, {}, false, false};
      laso_call_context context{sizeof(laso_call_context),
                                LASO_PLUGIN_ABI_VERSION,
                                &call,
                                call_should_stop,
                                write_json,
                                nullptr};
      auto input = r.input.dump();
      if (input.size() > std::size_t{1024} * 1024)
        throw Error(ErrorCode::Tool, "Plugin input exceeds limit");
      int32_t status = LASO_FAILED;
      try {
        status = registration_.invoke(registration_.instance, input.data(), input.size(), &context);
      } catch (...) {
        throw Error(ErrorCode::Tool, "Plugin invocation failed");
      }
      c.execution.check();
      if (status != LASO_OK || call.failed || !call.written)
        throw Error(ErrorCode::Tool, "Plugin invocation failed");
      auto output = Json::parse(call.output, nullptr, false);
      if (output.is_discarded())
        throw Error(ErrorCode::Tool, "Plugin returned invalid JSON");
      co_return ToolResult{std::move(output)};
    } catch (const Error &) {
      throw;
    } catch (...) {
      throw Error(ErrorCode::Tool, "Plugin invocation failed");
    }
  }

private:
  std::shared_ptr<Library> library_;
  ToolRegistration registration_;
  std::mutex mutex_;
};
class PluginModelProvider final : public ModelProvider {
public:
  PluginModelProvider(std::shared_ptr<Library> library, ProviderRegistration registration)
      : library_(std::move(library)), registration_(std::move(registration)) {}
  ProviderMetadata metadata() const override {
    return registration_.metadata;
  }
  ProviderHealth health() const override {
    if (!registration_.health)
      return {true, "plugin loaded; health callback not supplied"};
    try {
      std::lock_guard lock(mutex_);
      ExecutionContext execution{
          "", "", "plugin-health", {}, std::chrono::steady_clock::now() + Milliseconds{1000}};
      Call call{execution, {}, false, false};
      laso_call_context context{sizeof(laso_call_context),
                                LASO_PLUGIN_ABI_VERSION,
                                &call,
                                call_should_stop,
                                write_json,
                                nullptr};
      if (registration_.health(registration_.instance, &context) != LASO_OK || call.failed ||
          !call.written)
        return {false, "plugin health check failed"};
      const auto json = Json::parse(call.output, nullptr, false);
      if (json.is_discarded() || !json.is_object() || !json.contains("healthy") ||
          !json.at("healthy").is_boolean())
        return {false, "plugin health response is invalid"};
      return {json.at("healthy").get<bool>(), json.value("detail", std::string{})};
    } catch (...) {
      return {false, "plugin health check failed"};
    }
  }
  Task<ModelResponse> generate(const ModelRequest &request, ExecutionContext &execution) override {
    try {
      execution.check();
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock.owns_lock())
        throw Error(ErrorCode::Provider, "Plugin provider is busy");
      Call call{execution, {}, false, false};
      laso_call_context context{sizeof(laso_call_context),
                                LASO_PLUGIN_ABI_VERSION,
                                &call,
                                call_should_stop,
                                write_json,
                                nullptr};
      const Json input{{"operation", "generate"},
                       {"logical_model", registration_.metadata.name},
                       {"model", request.model},
                       {"prompt", request.prompt},
                       {"input", request.input},
                       {"options", request.options},
                       {"timeout_ms", std::chrono::duration_cast<Milliseconds>(
                                          execution.deadline - std::chrono::steady_clock::now())
                                          .count()}};
      const auto wire = input.dump();
      if (wire.size() > std::size_t{1024} * 1024)
        throw Error(ErrorCode::Provider, "Plugin provider input exceeds limit");
      int32_t status = LASO_FAILED;
      try {
        status = registration_.invoke(registration_.instance, wire.data(), wire.size(), &context);
      } catch (...) {
        throw Error(ErrorCode::Provider, "Plugin provider generation failed");
      }
      execution.check();
      if (status != LASO_OK || call.failed || !call.written)
        throw Error(ErrorCode::Provider, "Plugin provider generation failed");
      const auto response = Json::parse(call.output, nullptr, false);
      if (response.is_discarded() || !response.is_object())
        throw Error(ErrorCode::Provider, "Plugin provider returned invalid JSON");
      if (!response.value("ok", true))
        throw Error(ErrorCode::Provider, "Plugin provider reported a generation failure");
      if (!response.contains("output"))
        throw Error(ErrorCode::Provider, "Plugin provider response has no output");
      const auto model = response.value("model", request.model);
      const auto provider = response.value("provider", registration_.metadata.name);
      if ((response.contains("model") && !response.at("model").is_string()) ||
          (response.contains("provider") && !response.at("provider").is_string()))
        throw Error(ErrorCode::Provider, "Plugin provider response has invalid fields");
      co_return ModelResponse{response.at("output"), model, provider};
    } catch (const Error &) {
      throw;
    } catch (...) {
      throw Error(ErrorCode::Provider, "Plugin provider generation failed");
    }
  }

private:
  std::shared_ptr<Library> library_;
  ProviderRegistration registration_;
  mutable std::mutex mutex_;
};
std::string configuration_identity(const Json &config) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto byte : config.dump()) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return std::to_string(hash);
}
struct PluginLoader::EventSource {
  std::shared_ptr<Library> library;
  EventRegistration registration;
  PluginLoader::EventSubmitter submitter;
  PluginLoader::EventStatePersist persist;
  EventSourceInfo info;
  Json config = Json::object();
  mutable std::mutex mutex;
  std::atomic<bool> stop_requested{false};
  bool started = false;

  static int32_t should_stop(void *opaque) noexcept {
    try {
      return static_cast<EventSource *>(opaque)->stop_requested.load() ? 1 : 0;
    } catch (...) {
      return 1;
    }
  }
  static int32_t emit(void *opaque, const char *bytes, uint64_t length) noexcept {
    auto *source = static_cast<EventSource *>(opaque);
    try {
      if (!source)
        return LASO_INVALID;
      if (source->stop_requested.load())
        return LASO_STOPPED;
      if (!bytes || length > std::uint64_t{1024} * 1024) {
        std::lock_guard lock(source->mutex);
        ++source->info.rejected;
        ++source->info.malformed;
        return LASO_BUFFER_LIMIT;
      }
      if (!source->submitter) {
        std::lock_guard lock(source->mutex);
        ++source->info.rejected;
        return LASO_FAILED;
      }
      const auto result = source->submitter(source->info.id, source->info.plugin,
                                            source->info.component, source->info.event_schema,
                                            std::string(bytes, static_cast<std::size_t>(length)));
      std::lock_guard lock(source->mutex);
      switch (result.status) {
      case IngressStatus::Accepted:
        ++source->info.accepted;
        return LASO_OK;
      case IngressStatus::Duplicate:
        ++source->info.deduplicated;
        return LASO_DUPLICATE;
      case IngressStatus::Backpressured:
        ++source->info.backpressured;
        return LASO_BACKPRESSURE;
      case IngressStatus::Stopped:
        return LASO_STOPPED;
      case IngressStatus::Rejected:
        ++source->info.rejected;
        ++source->info.malformed;
        return LASO_INVALID;
      }
    } catch (...) {
      std::lock_guard lock(source->mutex);
      ++source->info.rejected;
      ++source->info.malformed;
      return LASO_FAILED;
    }
    return LASO_FAILED;
  }
  void save() {
    if (!persist)
      return;
    try {
      EventSourceInfo copy;
      {
        std::lock_guard lock(mutex);
        copy = info;
      }
      persist(copy);
    } catch (...) {
      log_diagnostic("event_source.state_persist_failed", {{"source_id", info.id}});
    }
  }
  void start() {
    {
      std::lock_guard lock(mutex);
      if (!info.enabled || started)
        return;
      stop_requested = false;
      info.status = "starting";
      info.healthy = false;
      started = true;
    }
    const auto wire = config.dump();
    laso_call_context context{
        sizeof(laso_call_context), LASO_PLUGIN_ABI_VERSION, this, &should_stop, nullptr, &emit};
    int32_t result = LASO_FAILED;
    try {
      result = registration.start(registration.instance, wire.data(), wire.size(), &context);
    } catch (...) {
      result = LASO_FAILED;
    }
    bool healthy = result == LASO_OK;
    if (result == LASO_OK && registration.health) {
      try {
        ExecutionContext execution{"",
                                   "",
                                   "event-source-health",
                                   {},
                                   std::chrono::steady_clock::now() + Milliseconds{1000}};
        Call call{execution, {}, false, false};
        laso_call_context health_context{sizeof(laso_call_context),
                                         LASO_PLUGIN_ABI_VERSION,
                                         &call,
                                         call_should_stop,
                                         write_json,
                                         nullptr};
        const auto health_result = registration.health(registration.instance, &health_context);
        const auto health = Json::parse(call.output, nullptr, false);
        healthy = health_result == LASO_OK && !call.failed && call.written &&
                  !health.is_discarded() && health.is_object() && health.value("healthy", false);
      } catch (...) {
        healthy = false;
      }
    }
    {
      std::lock_guard lock(mutex);
      if (result != LASO_OK) {
        started = false;
        info.status = "failed";
        info.healthy = false;
        ++info.rejected;
      } else {
        // A health probe describes a running source. A degraded source must
        // remain started so shutdown can still invoke its stop callback and
        // drain its producer threads safely.
        info.healthy = healthy;
        info.status = healthy ? "healthy" : "degraded";
      }
    }
    save();
    if (result != LASO_OK)
      log_diagnostic("event_source.failed", {{"source_id", info.id}});
  }
  void stop() noexcept {
    bool was_started = false;
    {
      std::lock_guard lock(mutex);
      was_started = started;
      stop_requested = true;
    }
    if (was_started) {
      laso_call_context context{
          sizeof(laso_call_context), LASO_PLUGIN_ABI_VERSION, this, &should_stop, nullptr, &emit};
      int32_t result = LASO_FAILED;
      try {
        result = registration.stop(registration.instance, &context);
      } catch (...) {
        result = LASO_FAILED;
      }
      std::lock_guard lock(mutex);
      started = false;
      info.healthy = false;
      info.status = result == LASO_OK ? "stopped" : "failed";
    } else {
      std::lock_guard lock(mutex);
      if (info.status != "disabled")
        info.status = "stopped";
      info.healthy = false;
    }
    save();
  }
  void set_enabled(bool value, bool loader_started) {
    {
      std::lock_guard lock(mutex);
      info.enabled = value;
    }
    if (value) {
      if (loader_started)
        start();
      else
        save();
    } else {
      stop();
      {
        std::lock_guard lock(mutex);
        info.status = "disabled";
      }
      save();
    }
  }
  Json json() const {
    std::lock_guard lock(mutex);
    return Json(info);
  }
};
PluginLoader::~PluginLoader() noexcept {
  stop_event_sources();
}
void PluginLoader::discover(const std::vector<std::filesystem::path> &directories,
                            const std::map<std::string, EventSourceConfig> &event_sources) {
  event_configs_ = event_sources;
  std::set<std::filesystem::path> paths;
  for (const auto &directory : directories) {
    std::error_code ec;
    auto canonical = std::filesystem::canonical(directory, ec);
    if (ec || !std::filesystem::is_directory(canonical))
      throw Error(ErrorCode::Configuration, "Configured plugin directory is unavailable");
    std::filesystem::directory_iterator entries(canonical, ec);
    if (ec)
      throw Error(ErrorCode::Configuration, "Configured plugin directory cannot be read");
    for (const auto &entry : entries) {
      // Do not follow symlinks outside a configured directory (or load versioned .so.* files).
      if (entry.is_symlink() || !entry.is_regular_file() || entry.path().extension() != ".so")
        continue;
      paths.insert(entry.path());
    }
  }
  for (const auto &path : paths)
    load(path);
  for (const auto &[id, config] : event_configs_)
    if (!matched_event_configs_.contains(id))
      throw Error(ErrorCode::Configuration, "Configured event source was not found",
                  {{"source_id", id}, {"plugin", config.plugin}});
}
void PluginLoader::load(const std::filesystem::path &path) {
  PluginInfo info;
  info.path = path.string();
  try {
    auto library = std::make_shared<Library>();
    library->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library->library)
      throw Error(ErrorCode::Plugin, "Cannot load plugin shared object");
    auto query = reinterpret_cast<const laso_plugin_descriptor *(*)()>(
        dlsym(library->library, "laso_plugin_query"));
    auto init = reinterpret_cast<int32_t (*)(const laso_host_api *, laso_plugin_handle *)>(
        dlsym(library->library, "laso_plugin_init"));
    library->shutdown = reinterpret_cast<void (*)(laso_plugin_handle)>(
        dlsym(library->library, "laso_plugin_shutdown"));
    if (!query || !init || !library->shutdown)
      throw Error(ErrorCode::Plugin, "Missing ABI symbols");
    auto descriptor = query();
    if (!descriptor || descriptor->struct_size < sizeof(laso_plugin_descriptor))
      throw Error(ErrorCode::Plugin, "Invalid ABI descriptor");
    info.abi = descriptor->abi_version;
    if (info.abi != LASO_PLUGIN_ABI_VERSION)
      throw Error(ErrorCode::Plugin, "Incompatible plugin ABI");
    info.name = bounded(descriptor->name, 128);
    info.version = bounded(descriptor->version, 64);
    (void)bounded(descriptor->description, 4096);
    for (const auto &existing : plugins_)
      if (existing.loaded && existing.name == info.name)
        throw Error(ErrorCode::Plugin, "Duplicate plugin name");
    Staging staging;
    laso_host_api host{sizeof(laso_host_api), LASO_PLUGIN_ABI_VERSION, &staging,
                       register_component};
    auto status = init(&host, &library->handle);
    if (status != LASO_OK || !library->handle || staging.failed)
      throw Error(ErrorCode::Plugin, "Plugin initialization failed");
    std::set<std::string> names;
    for (const auto &name : tools_.names())
      names.insert(name);
    for (const auto &r : staging.tools)
      if (!names.insert(r.metadata.name).second)
        throw Error(ErrorCode::Plugin, "Duplicate component registration");
    std::set<std::string> provider_names;
    for (const auto &name : providers_.names())
      provider_names.insert(name);
    for (const auto &r : staging.providers)
      if (!provider_names.insert(r.metadata.name).second)
        throw Error(ErrorCode::Plugin, "Duplicate provider registration");
    std::set<std::string> source_ids;
    for (const auto &source : event_sources_)
      source_ids.insert(source->info.id);
    std::vector<std::shared_ptr<EventSource>> source_batch;
    for (const auto &registration : staging.events) {
      const EventSourceConfig *configuration = nullptr;
      std::string source_id;
      for (const auto &[id, candidate] : event_configs_)
        if (candidate.plugin == info.name &&
            (candidate.component.empty() || candidate.component == registration.name)) {
          if (configuration)
            throw Error(ErrorCode::Configuration, "Multiple event sources select one component");
          configuration = &candidate;
          source_id = id;
        }
      if (!configuration)
        source_id = info.name + "." + registration.name;
      if (!source_ids.insert(source_id).second)
        throw Error(ErrorCode::Plugin, "Duplicate event source identity");
      EventSourceInfo source_info;
      source_info.id = source_id;
      source_info.plugin = info.name;
      source_info.plugin_version = info.version;
      source_info.component = registration.name;
      source_info.event_schema = configuration && !configuration->schema.empty()
                                     ? configuration->schema
                                     : registration.event_schema;
      source_info.capabilities = registration.capabilities;
      source_info.configuration_identity = configuration
                                               ? configuration_identity(configuration->config)
                                               : configuration_identity(Json::object());
      source_info.enabled = configuration && configuration->enabled;
      if (event_state_load_) {
        try {
          if (auto previous = event_state_load_(source_id); previous.has_value())
            source_info.enabled = previous->enabled;
        } catch (...) {
          throw Error(ErrorCode::Storage, "Cannot load event source state");
        }
      }
      auto source = std::make_shared<EventSource>();
      source->library = library;
      source->registration = registration;
      source->submitter = event_submitter_;
      source->persist = event_state_persist_;
      source->info = std::move(source_info);
      source->config = configuration ? configuration->config : Json::object();
      source_batch.push_back(std::move(source));
    }
    // Stage all wrappers before installing the batch, retaining the library through wrappers.
    std::map<std::string, std::shared_ptr<Tool>> batch;
    for (auto &r : staging.tools) {
      r.metadata.plugin = info.name;
      auto name = r.metadata.name;
      batch.emplace(name, std::make_shared<PluginTool>(library, std::move(r)));
    }
    std::map<std::string, std::shared_ptr<ModelProvider>> provider_batch;
    for (auto &r : staging.providers) {
      r.metadata.plugin = info.name;
      auto name = r.metadata.name;
      provider_batch.emplace(name, std::make_shared<PluginModelProvider>(library, std::move(r)));
    }
    tools_.add_batch(batch);
    providers_.add_batch(provider_batch);
    libraries_.push_back(library);
    for (const auto &source : source_batch) {
      if (event_configs_.contains(source->info.id))
        matched_event_configs_.insert(source->info.id);
      event_sources_.push_back(source);
      source->save();
    }
    info.loaded = true;
  } catch (const Error &e) {
    info.error = e.what();
  } catch (...) {
    info.error = "Plugin load failed";
  }
  // PluginInfo.path and exception text may contain deployment-specific details;
  // keep them available to the API but out of the default diagnostic log.
  log_diagnostic(info.loaded ? "plugin.loaded" : "plugin.failed",
                 {{"plugin", info.name}, {"loaded", info.loaded}, {"abi", info.abi}});
  plugins_.push_back(std::move(info));
}
void PluginLoader::start_event_sources() {
  std::lock_guard lock(lifecycle_mutex_);
  event_sources_started_ = true;
  for (const auto &source : event_sources_)
    source->start();
}
void PluginLoader::stop_event_sources() noexcept {
  std::lock_guard lock(lifecycle_mutex_);
  for (auto it = event_sources_.rbegin(); it != event_sources_.rend(); ++it)
    (*it)->stop();
  event_sources_started_ = false;
}
void PluginLoader::set_event_source_enabled(const std::string &id, bool enabled) {
  std::lock_guard lock(lifecycle_mutex_);
  for (const auto &source : event_sources_)
    if (source->info.id == id) {
      source->set_enabled(enabled, event_sources_started_);
      return;
    }
  throw Error(ErrorCode::NotFound, "Event source is not registered", {{"source_id", id}});
}
Json PluginLoader::event_sources() const {
  Json result = Json::array();
  for (const auto &source : event_sources_)
    result.push_back(source->json());
  return result;
}
Json PluginLoader::event_source(const std::string &id) const {
  for (const auto &source : event_sources_)
    if (source->info.id == id)
      return source->json();
  throw Error(ErrorCode::NotFound, "Event source is not registered", {{"source_id", id}});
}
} // namespace laso
