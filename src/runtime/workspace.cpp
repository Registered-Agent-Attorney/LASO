#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <laso/runtime/workspace.hpp>
#include <set>
#include <system_error>

namespace laso {
namespace {
constexpr std::array<std::uint32_t, 64> round_constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
  return (value >> count) | (value << (32 - count));
}
std::string sha256(const std::vector<unsigned char> &input) {
  std::vector<unsigned char> bytes = input;
  const auto bit_count = static_cast<std::uint64_t>(bytes.size()) * 8;
  bytes.push_back(0x80);
  while ((bytes.size() % 64) != 56)
    bytes.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<unsigned char>((bit_count >> shift) & 0xff));
  std::array<std::uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
    std::array<std::uint32_t, 64> schedule{};
    for (unsigned i = 0; i < 16; ++i)
      for (unsigned byte = 0; byte < 4; ++byte)
        schedule[i] = (schedule[i] << 8) | bytes[offset + i * 4 + byte];
    for (unsigned i = 16; i < 64; ++i) {
      const auto s0 = rotate_right(schedule[i - 15], 7) ^ rotate_right(schedule[i - 15], 18) ^
                      (schedule[i - 15] >> 3);
      const auto s1 = rotate_right(schedule[i - 2], 17) ^ rotate_right(schedule[i - 2], 19) ^
                      (schedule[i - 2] >> 10);
      schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    auto working = hash;
    for (unsigned i = 0; i < 64; ++i) {
      const auto s1 =
          rotate_right(working[4], 6) ^ rotate_right(working[4], 11) ^ rotate_right(working[4], 25);
      const auto choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
      const auto temp1 = working[7] + s1 + choice + round_constants[i] + schedule[i];
      const auto s0 =
          rotate_right(working[0], 2) ^ rotate_right(working[0], 13) ^ rotate_right(working[0], 22);
      const auto majority =
          (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
      const auto temp2 = s0 + majority;
      working[7] = working[6];
      working[6] = working[5];
      working[5] = working[4];
      working[4] = working[3] + temp1;
      working[3] = working[2];
      working[2] = working[1];
      working[1] = working[0];
      working[0] = temp1 + temp2;
    }
    for (unsigned i = 0; i < 8; ++i)
      hash[i] += working[i];
  }
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto value : hash)
    for (int shift = 28; shift >= 0; shift -= 4)
      result.push_back(hex[(value >> shift) & 0xf]);
  return result;
}

bool safe_relative_path(const std::string &value) {
  if (value.empty() || value.size() > 512 || value.front() == '/' || value.front() == '\\' ||
      value.find(':') != std::string::npos || value.find('\\') != std::string::npos ||
      value.find('\0') != std::string::npos)
    return false;
  const auto path = std::filesystem::path(value);
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
    return false;
  for (const auto &part : path)
    if (part == ".." || part == ".")
      return false;
  return true;
}

bool safe_component(const std::string &value) {
  return !value.empty() && value.size() <= 128 && value != "." && value != ".." &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':';
         });
}

std::vector<unsigned char> read_file(const std::filesystem::path &path, std::size_t limit) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw Error(ErrorCode::Storage, "Unable to read workspace file");
  stream.seekg(0, std::ios::end);
  const auto size = stream.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > limit)
    throw Error(ErrorCode::Validation, "Workspace file exceeds the configured limit");
  stream.seekg(0, std::ios::beg);
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty() && !stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
    throw Error(ErrorCode::Storage, "Unable to read workspace file");
  return bytes;
}

void check_manifest_size(const Json &manifest, const WorkspaceManifestLimits &limits) {
  if (!manifest.is_object() || manifest.value("version", 0U) != 1 || !manifest.contains("files") ||
      !manifest.at("files").is_array() || manifest.dump().size() > limits.max_manifest_bytes)
    throw Error(ErrorCode::Validation, "Invalid or oversized workspace manifest");
}
} // namespace

void validate_workspace_manifest(const Json &manifest, WorkspaceManifestLimits limits) {
  check_manifest_size(manifest, limits);
  if (manifest.at("files").size() > limits.max_files)
    throw Error(ErrorCode::Validation, "Workspace file count exceeds the configured limit");
  std::set<std::string> paths;
  std::size_t total = 0;
  for (const auto &entry : manifest.at("files")) {
    if (!entry.is_object() || !entry.contains("path") || !entry.at("path").is_string() ||
        !safe_relative_path(entry.at("path").get<std::string>()) || !entry.contains("data") ||
        !entry.at("data").is_array() || !entry.contains("sha256") ||
        !entry.at("sha256").is_string())
      throw Error(ErrorCode::Validation, "Invalid workspace manifest entry");
    const auto path = entry.at("path").get<std::string>();
    if (!paths.insert(path).second)
      throw Error(ErrorCode::Conflict, "Workspace manifest contains duplicate paths");
    const auto bytes = entry.at("data").get<std::vector<unsigned char>>();
    if (bytes.size() > limits.max_file_bytes || total > limits.max_total_bytes - bytes.size() ||
        entry.at("sha256") != sha256(bytes))
      throw Error(ErrorCode::Validation, "Workspace manifest integrity or size check failed");
    total += bytes.size();
  }
}

Json workspace_manifest(const std::filesystem::path &root, WorkspaceManifestLimits limits) {
  std::error_code error;
  const auto canonical_root = std::filesystem::canonical(root, error);
  if (error || !std::filesystem::is_directory(canonical_root, error))
    throw Error(ErrorCode::Validation, "Workspace root is not a directory");
  Json result{{"version", 1}, {"files", Json::array()}};
  std::size_t total = 0;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           canonical_root, std::filesystem::directory_options::none, error)) {
    if (error)
      throw Error(ErrorCode::Storage, "Unable to enumerate workspace");
    if (entry.is_symlink(error))
      throw Error(ErrorCode::Validation, "Workspace symlinks are not permitted");
    if (!entry.is_regular_file(error)) {
      if (error)
        throw Error(ErrorCode::Storage, "Unable to inspect workspace entry");
      continue;
    }
    if (result["files"].size() >= limits.max_files)
      throw Error(ErrorCode::Validation, "Workspace file count exceeds the configured limit");
    const auto relative =
        std::filesystem::relative(entry.path(), canonical_root, error).generic_string();
    if (error || !safe_relative_path(relative))
      throw Error(ErrorCode::Validation, "Workspace contains an unsafe relative path");
    const auto bytes = read_file(entry.path(), limits.max_file_bytes);
    if (total > limits.max_total_bytes - bytes.size())
      throw Error(ErrorCode::Validation, "Workspace exceeds the configured byte limit");
    total += bytes.size();
    result["files"].push_back(
        {{"path", relative}, {"size", bytes.size()}, {"sha256", sha256(bytes)}, {"data", bytes}});
  }
  validate_workspace_manifest(result, limits);
  return result;
}

std::filesystem::path stage_workspace(const Json &manifest,
                                      const std::filesystem::path &staging_root,
                                      const std::string &run_id, const std::string &work_id,
                                      const std::string &attempt_id,
                                      WorkspaceManifestLimits limits) {
  validate_workspace_manifest(manifest, limits);
  if (!safe_component(run_id) || !safe_component(work_id) || !safe_component(attempt_id))
    throw Error(ErrorCode::Validation, "Workspace staging identity is incomplete");
  const auto stage =
      staging_root / ("run-" + run_id) / ("work-" + work_id) / ("attempt-" + attempt_id);
  std::error_code error;
  std::filesystem::remove_all(stage, error);
  if (error || !std::filesystem::create_directories(stage, error) || error)
    throw Error(ErrorCode::Storage, "Unable to create workspace staging directory");
  for (const auto &entry : manifest.at("files")) {
    const auto relative = std::filesystem::path(entry.at("path").get<std::string>());
    const auto destination = stage / relative;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
      throw Error(ErrorCode::Storage, "Unable to create workspace parent directory");
    const auto bytes = entry.at("data").get<std::vector<unsigned char>>();
    std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
    if (!stream || (!bytes.empty() &&
                    !stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size())))
      throw Error(ErrorCode::Storage, "Unable to stage workspace file");
  }
  return stage;
}
} // namespace laso
