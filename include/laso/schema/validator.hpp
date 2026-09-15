#pragma once
#include <filesystem>
#include <laso/core/types.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace laso {
struct SchemaLimits {
  std::size_t max_schema_bytes = std::size_t{1024} * 1024;
  std::size_t max_payload_bytes = std::size_t{1024} * 1024;
  unsigned max_depth = 64;
  unsigned max_reference_documents = 64;
  std::size_t max_cached_schemas = 256;
};

class SchemaValidator {
public:
  explicit SchemaValidator(std::vector<std::filesystem::path> roots = {}, SchemaLimits limits = {});

  void validate(const std::string &reference, const Json &payload, const std::string &node_id,
                const std::string &direction) const;
  void validate_declaration(const std::string &reference) const;

private:
  struct Loaded;
  std::vector<std::filesystem::path> roots_;
  SchemaLimits limits_;
  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<std::string, std::shared_ptr<const Loaded>> cache_;

  std::filesystem::path resolve_root_reference(const std::string &) const;
  std::filesystem::path resolve_uri(const std::string &, const std::filesystem::path &) const;
  std::shared_ptr<const Loaded> load(const std::filesystem::path &) const;
  void inspect_schema(const Json &, const std::filesystem::path &, std::set<std::string> &,
                      std::set<std::string> &, unsigned, unsigned &) const;
};
} // namespace laso
