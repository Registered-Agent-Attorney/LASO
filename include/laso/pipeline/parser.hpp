#pragma once
#include <filesystem>
#include <laso/core/types.hpp>
#include <set>

namespace laso {
inline constexpr std::size_t max_document_bytes = std::size_t{1024} * 1024;
struct PipelineReference {
  std::string name;
  unsigned version = 0;
  bool explicit_version = false;
};
std::string read_document(const std::filesystem::path &path);
PipelineReference parse_pipeline_reference(const std::string &text);
std::string pipeline_reference(const std::string &name, unsigned version);
PipelineDefinition parse_pipeline(const std::string &text,
                                  const std::set<std::string> &extensions = {});
void validate_pipeline(const PipelineDefinition &pipeline,
                       const std::set<std::string> &extensions = {});
} // namespace laso
