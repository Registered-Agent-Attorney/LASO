#pragma once
#include <cstddef>
#include <filesystem>
#include <laso/core/types.hpp>

namespace laso {
struct WorkspaceManifestLimits {
  std::size_t max_files = 64;
  std::size_t max_file_bytes = 16 * 1024;
  std::size_t max_total_bytes = 48 * 1024;
  std::size_t max_manifest_bytes = 64 * 1024;
};

// The inline manifest format is deliberately bounded for the first remote
// worker milestone.  It contains relative paths and byte arrays only; no host
// paths, symlinks, or credentials are transported.
Json workspace_manifest(const std::filesystem::path &root,
                        WorkspaceManifestLimits limits = {});
std::filesystem::path stage_workspace(const Json &manifest,
                                      const std::filesystem::path &staging_root,
                                      const std::string &run_id, const std::string &work_id,
                                      const std::string &attempt_id,
                                      WorkspaceManifestLimits limits = {});
void validate_workspace_manifest(const Json &manifest,
                                 WorkspaceManifestLimits limits = {});
} // namespace laso
