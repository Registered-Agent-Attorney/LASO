#include <cstring>
#include <dlfcn.h>
#include <laso/core/config.hpp>
#include <laso/plugins/loader.hpp>
#include <laso_plugin.h>
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
struct Registration {
  ToolMetadata metadata;
  void *instance;
  laso_invoke_fn invoke;
};
struct Staging {
  std::vector<Registration> registrations;
  bool failed = false;
};
int32_t register_component(void *opaque, const laso_component *c) noexcept {
  auto &staging = *static_cast<Staging *>(opaque);
  try {
    if (!c || c->struct_size < sizeof(laso_component) || !c->invoke)
      throw Error(ErrorCode::Plugin, "Invalid component");
    if (c->kind != LASO_COMPONENT_TOOL) {
      staging.failed = true;
      return LASO_UNSUPPORTED;
    }
    if (staging.registrations.size() >= 64)
      throw Error(ErrorCode::Plugin, "Too many plugin registrations");
    ToolMetadata m;
    m.name = bounded(c->name, 128);
    if (!std::regex_match(m.name, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")))
      throw Error(ErrorCode::Plugin, "Invalid component name");
    auto json = Json::parse(bounded(c->metadata_json, 16384));
    m.description = json.value("description", std::string{});
    m.network = json.value("network", false);
    m.approval_required = json.value("approval_required", false);
    m.permission = json.value("permission", std::string("local"));
    m.input_schema = json.value("input_schema", Json::object());
    m.output_schema = json.value("output_schema", Json::object());
    auto timeout = json.value("timeout_ms", 30000);
    if (timeout < 1 || timeout > 3600000)
      throw Error(ErrorCode::Plugin, "Invalid component timeout");
    m.timeout = Milliseconds(timeout);
    staging.registrations.push_back({std::move(m), c->instance, c->invoke});
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
  PluginTool(std::shared_ptr<Library> library, Registration registration)
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
  Registration registration_;
  std::mutex mutex_;
};
} // namespace
void PluginLoader::discover(const std::vector<std::filesystem::path> &directories) {
  std::set<std::filesystem::path> paths;
  for (const auto &directory : directories) {
    auto canonical = std::filesystem::canonical(directory);
    for (const auto &entry : std::filesystem::directory_iterator(canonical)) {
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
    for (const auto &r : staging.registrations)
      if (!names.insert(r.metadata.name).second)
        throw Error(ErrorCode::Plugin, "Duplicate component registration");
    // Stage all wrappers before installing the batch, retaining the library through wrappers.
    std::map<std::string, std::shared_ptr<Tool>> batch;
    for (auto &r : staging.registrations) {
      r.metadata.plugin = info.name;
      auto name = r.metadata.name;
      batch.emplace(name, std::make_shared<PluginTool>(library, std::move(r)));
    }
    tools_.add_batch(batch);
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
