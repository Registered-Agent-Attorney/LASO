#include <array>
#include <laso/storage/storage.hpp>
#include <mutex>
#include <sqlite3.h>

namespace laso {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
std::string table(RecordKind kind) {
  static constexpr std::array names = {"pipelines", "runs",      "attempts", "messages",
                                       "approvals", "artifacts", "events"};
  return names.at(static_cast<std::size_t>(kind));
}
void exec(sqlite3 *db, const std::string &sql) {
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
    throw Error(ErrorCode::Storage, "SQLite operation failed");
}
Statement prepare(sqlite3 *db, const std::string &sql) {
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    throw Error(ErrorCode::Storage, "SQLite statement failed");
  return {raw, sqlite3_finalize};
}
void bind(sqlite3_stmt *s, int index, const std::string &value) {
  if (sqlite3_bind_text(s, index, value.c_str(), static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK)
    throw Error(ErrorCode::Storage, "SQLite binding failed");
}
Json parse(sqlite3_stmt *s) {
  const auto *data = sqlite3_column_text(s, 0);
  if (!data)
    throw Error(ErrorCode::Storage, "Missing SQLite record");
  try {
    return Json::parse(reinterpret_cast<const char *>(data));
  } catch (const Json::exception &) {
    throw Error(ErrorCode::Storage, "Invalid stored record");
  }
}
} // namespace
struct SQLiteStorage::Impl {
  std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db{nullptr, sqlite3_close_v2};
  mutable std::mutex mutex;
};
SQLiteStorage::SQLiteStorage(const std::filesystem::path &path) : impl_(std::make_unique<Impl>()) {
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path());
  sqlite3 *raw = nullptr;
  const auto result =
      sqlite3_open_v2(path.c_str(), &raw,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  impl_->db.reset(raw);
  if (result != SQLITE_OK)
    throw Error(ErrorCode::Storage, "Cannot open SQLite database");
  sqlite3_busy_timeout(raw, 5000);
  exec(raw, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
  auto version_stmt = prepare(raw, "PRAGMA user_version");
  if (sqlite3_step(version_stmt.get()) != SQLITE_ROW ||
      sqlite3_column_int(version_stmt.get(), 0) > 1)
    throw Error(ErrorCode::Storage, "Unsupported database schema version");
  version_stmt.reset();
  exec(raw, "BEGIN IMMEDIATE");
  try {
    for (int i = 0; i < 7; ++i) {
      auto name = table(static_cast<RecordKind>(i));
      exec(raw, "CREATE TABLE IF NOT EXISTS " + name +
                    " (id TEXT PRIMARY KEY, run_id TEXT NOT NULL, body TEXT NOT NULL "
                    "CHECK(json_valid(body)), sequence INTEGER NOT NULL)");
      exec(raw, "CREATE INDEX IF NOT EXISTS " + name + "_run ON " + name + "(run_id,sequence)");
    }
    exec(raw, "PRAGMA user_version=1; COMMIT");
  } catch (...) {
    sqlite3_exec(raw, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}
SQLiteStorage::~SQLiteStorage() = default;
void SQLiteStorage::commit(const std::vector<Record> &records) {
  std::lock_guard lock(impl_->mutex);
  auto *db = impl_->db.get();
  exec(db, "BEGIN IMMEDIATE");
  try {
    for (const auto &r : records) {
      auto name = table(r.kind);
      auto s = prepare(
          db,
          "INSERT INTO " + name +
              "(id,run_id,body,sequence) VALUES(?,?,?,(SELECT COALESCE(MAX(sequence),0)+1 FROM " +
              name + ")) ON CONFLICT(id) DO UPDATE SET body=excluded.body,run_id=excluded.run_id");
      const auto body = r.value.dump();
      if (body.size() > 4 * 1024 * 1024)
        throw Error(ErrorCode::Storage, "Stored record exceeds limit");
      bind(s.get(), 1, r.id);
      bind(s.get(), 2, r.run_id);
      bind(s.get(), 3, body);
      if (sqlite3_step(s.get()) != SQLITE_DONE)
        throw Error(ErrorCode::Storage, "SQLite checkpoint failed");
    }
    exec(db, "COMMIT");
  } catch (...) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}
Json SQLiteStorage::get(RecordKind kind, const std::string &id) const {
  std::lock_guard lock(impl_->mutex);
  auto s = prepare(impl_->db.get(), "SELECT body FROM " + table(kind) + " WHERE id=?");
  bind(s.get(), 1, id);
  const auto rc = sqlite3_step(s.get());
  if (rc == SQLITE_DONE)
    throw Error(ErrorCode::NotFound, "Record not found");
  if (rc != SQLITE_ROW)
    throw Error(ErrorCode::Storage, "SQLite read failed");
  return parse(s.get());
}
std::vector<Json> SQLiteStorage::list(RecordKind kind, const std::string &run_id, std::size_t limit,
                                      std::size_t offset) const {
  if (limit > 10000 || offset > 100000000)
    throw Error(ErrorCode::Validation, "Pagination limit exceeded");
  std::lock_guard lock(impl_->mutex);
  auto s = prepare(impl_->db.get(), "SELECT body FROM " + table(kind) +
                                        (run_id.empty() ? "" : " WHERE run_id=?") +
                                        " ORDER BY sequence LIMIT ? OFFSET ?");
  int index = 1;
  if (!run_id.empty())
    bind(s.get(), index++, run_id);
  sqlite3_bind_int64(s.get(), index++, static_cast<sqlite3_int64>(limit));
  sqlite3_bind_int64(s.get(), index, static_cast<sqlite3_int64>(offset));
  std::vector<Json> result;
  int rc;
  while ((rc = sqlite3_step(s.get())) == SQLITE_ROW)
    result.push_back(parse(s.get()));
  if (rc != SQLITE_DONE)
    throw Error(ErrorCode::Storage, "SQLite list failed");
  return result;
}
} // namespace laso
