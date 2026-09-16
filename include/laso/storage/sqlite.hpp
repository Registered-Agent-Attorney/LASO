#pragma once
#include <filesystem>
#include <laso/storage/storage.hpp>
#include <memory>

namespace laso {
class SQLiteStorage final : public Storage {
public:
  explicit SQLiteStorage(const std::filesystem::path &path);
  ~SQLiteStorage() override;
  SQLiteStorage(const SQLiteStorage &) = delete;
  SQLiteStorage &operator=(const SQLiteStorage &) = delete;
  void commit(const std::vector<Record> &) override;
  Json get(RecordKind, const std::string &) const override;
  std::vector<Json> list(RecordKind, const std::string &run_id = "", std::size_t limit = 1000,
                         std::size_t offset = 0) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace laso
