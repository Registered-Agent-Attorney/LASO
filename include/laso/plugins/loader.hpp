#pragma once
#include <filesystem>
#include <laso/tools/tool.hpp>

namespace laso {
struct PluginInfo {
  std::string path, name, version, error;
  unsigned abi = 0;
  bool loaded = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PluginInfo, path, name, version, error, abi, loaded)
class PluginLoader {
public:
  explicit PluginLoader(ToolRegistry &tools) : tools_(tools) {}
  // Call during startup, before workers or consumers access the registries.
  void discover(const std::vector<std::filesystem::path> &configured_directories);
  const std::vector<PluginInfo> &plugins() const {
    return plugins_;
  }

private:
  ToolRegistry &tools_;
  std::vector<PluginInfo> plugins_;
  std::vector<std::shared_ptr<void>> libraries_;
  void load(const std::filesystem::path &);
};
} // namespace laso
