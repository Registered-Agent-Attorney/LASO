#pragma once
#include <filesystem>
#include <laso/core/types.hpp>
#include <memory>

namespace laso {
enum class RecordKind { Pipeline, Run, Attempt, Message, Approval, Artifact, Event };
struct Record {
  RecordKind kind;
  std::string id, run_id;
  Json value;
};
class Storage {
public:
  virtual ~Storage() = default;
  // An entire checkpoint commits atomically, or none of it does.
  virtual void commit(const std::vector<Record> &records) = 0;
  virtual Json get(RecordKind kind, const std::string &id) const = 0;
  virtual std::vector<Json> list(RecordKind kind, const std::string &run_id = "",
                                 std::size_t limit = 1000, std::size_t offset = 0) const = 0;
};
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
