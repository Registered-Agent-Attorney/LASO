#pragma once
#include <filesystem>
#include <laso/core/types.hpp>
#include <set>

namespace laso {
inline constexpr std::size_t max_document_bytes = 1024 * 1024;
std::string read_document(const std::filesystem::path &path);
PipelineDefinition parse_pipeline(const std::string &text,
                                  const std::set<std::string> &extensions = {});
void validate_pipeline(const PipelineDefinition &pipeline,
                       const std::set<std::string> &extensions = {});
} // namespace laso
