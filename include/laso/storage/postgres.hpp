#pragma once
#include <laso/storage/storage.hpp>
#include <memory>
#include <string>

namespace laso {
class PostgresStorage final : public Storage {
public:
  PostgresStorage(const std::string &dsn, const std::string &schema = "public");
  ~PostgresStorage() override;
  PostgresStorage(const PostgresStorage &) = delete;
  PostgresStorage &operator=(const PostgresStorage &) = delete;
  void commit(const std::vector<Record> &) override;
  Json get(RecordKind, const std::string &) const override;
  std::vector<Json> list(RecordKind, const std::string &run_id = "", std::size_t limit = 1000,
                         std::size_t offset = 0) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace laso
