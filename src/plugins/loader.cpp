#include <cstring>
#include <dlfcn.h>
#include <laso/core/config.hpp>
#include <laso/plugins/loader.hpp>
#include <laso_plugin.h>
#include <cstddef>
#include <mutex>
#include <regex>
#include <set>

namespace laso {
namespace {
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
    if (shutdown && handle)
      shutdown(handle);
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
struct Staging {
  std::vector<ToolRegistration> tools;
  std::vector<ProviderRegistration> providers;
  bool failed = false;
};
int32_t register_component(void *opaque, const laso_component *c) noexcept {
  auto &staging = *static_cast<Staging *>(opaque);
  try {
    constexpr auto required_size = offsetof(laso_component, health);
    if (!c || c->struct_size < required_size || !c->invoke)
      throw Error(ErrorCode::Plugin, "Invalid component");
    if (c->kind != LASO_COMPONENT_TOOL && c->kind != LASO_COMPONENT_MODEL) {
      staging.failed = true;
      return LASO_UNSUPPORTED;
    }
    if (staging.tools.size() + staging.providers.size() >= 64)
      throw Error(ErrorCode::Plugin, "Too many plugin registrations");
    const auto name = bounded(c->name, 128);
    if (!std::regex_match(name, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")))
      throw Error(ErrorCode::Plugin, "Invalid component name");
    auto json = Json::parse(bounded(c->metadata_json, 16384));
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
int32_t should_stop(void *opaque) noexcept {
  auto &call = *static_cast<Call *>(opaque);
  return call.context.stop.stop_requested() ||
         std::chrono::steady_clock::now() >= call.context.deadline;
}
int32_t write_json(void *opaque, const char *bytes, uint64_t length) noexcept {
  auto &call = *static_cast<Call *>(opaque);
  try {
    if (!bytes || length > 1024 * 1024 || call.written) {
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
    c.execution.check();
    // ABI v1 callbacks are short, cooperative local calls, serialized per component.
    // Remote/nonblocking provider adapters belong in a future async ABI revision.
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
      throw Error(ErrorCode::Tool, "Plugin component is busy");
    Call call{c.execution, {}, false, false};
    laso_call_context context{sizeof(laso_call_context), LASO_PLUGIN_ABI_VERSION, &call,
                              should_stop, write_json};
    auto input = r.input.dump();
    auto status =
        registration_.invoke(registration_.instance, input.data(), input.size(), &context);
    c.execution.check();
    if (status != LASO_OK || call.failed || !call.written)
      throw Error(ErrorCode::Tool, "Plugin invocation failed");
    auto output = Json::parse(call.output, nullptr, false);
    if (output.is_discarded())
      throw Error(ErrorCode::Tool, "Plugin returned invalid JSON");
    co_return ToolResult{std::move(output)};
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
      ExecutionContext execution{"", "", "plugin-health", {},
                                 std::chrono::steady_clock::now() + Milliseconds{1000}};
      Call call{execution, {}, false, false};
      laso_call_context context{sizeof(laso_call_context), LASO_PLUGIN_ABI_VERSION, &call,
                                should_stop, write_json};
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
    execution.check();
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
      throw Error(ErrorCode::Provider, "Plugin provider is busy");
    Call call{execution, {}, false, false};
    laso_call_context context{sizeof(laso_call_context), LASO_PLUGIN_ABI_VERSION, &call,
                              should_stop, write_json};
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
    const auto status = registration_.invoke(registration_.instance, wire.data(), wire.size(), &context);
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
  }

private:
  std::shared_ptr<Library> library_;
  ProviderRegistration registration_;
  mutable std::mutex mutex_;
};
} // namespace
void PluginLoader::discover(const std::vector<std::filesystem::path> &directories) {
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
    info.loaded = true;
  } catch (const Error &e) {
    info.error = e.what();
  } catch (...) {
    info.error = "Plugin load failed";
  }
  log_diagnostic(info.loaded ? "plugin.loaded" : "plugin.failed", Json(info));
  plugins_.push_back(std::move(info));
}
} // namespace laso
