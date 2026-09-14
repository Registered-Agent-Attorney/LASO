#pragma once
#include <string>
#include <yaml-cpp/yaml.h>
namespace laso::detail {
// Parsing adapters only: raw YAML never enters the runtime or domain model.
YAML::Node load_safe_yaml(const std::string &text);
} // namespace laso::detail
