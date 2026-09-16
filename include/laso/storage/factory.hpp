#pragma once
#include <filesystem>
#include <laso/storage/storage.hpp>
#include <memory>
#include <string>

namespace laso {
struct StorageOptions {
  std::string backend = "sqlite";
  std::filesystem::path db_path;
};

std::unique_ptr<Storage> create_storage(const StorageOptions &options);
} // namespace laso
