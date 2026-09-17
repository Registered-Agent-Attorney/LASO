#pragma once
#include <filesystem>
#include <laso/core/types.hpp>
#include <memory>
#include <string>
#include <vector>

namespace laso {
enum class RecordKind {
  Pipeline,
  Run,
  Attempt,
  Message,
  Approval,
  Artifact,
  Event,
  Schedule,
  Trigger,
  ScheduleOccurrence,
  TriggerDelivery,
  EventSource,
  ExternalEventClaim,
  WorkerJob,
  WorkerInteraction
};
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
  // Atomically inserts a durable claim.  An existing id is never overwritten.
  // This is used for schedule occurrences, event-trigger deliveries, and
  // external event identities. Associated records are inserted in the same
  // transaction only when the claim is new.
  virtual bool claim(const Record &record, const std::vector<Record> &associated = {}) = 0;
  virtual Json get(RecordKind kind, const std::string &id) const = 0;
  virtual std::vector<Json> list(RecordKind kind, const std::string &run_id = "",
                                 std::size_t limit = 1000, std::size_t offset = 0) const = 0;
};
// Kept in this long-standing public header for source compatibility. New code
// may include <laso/storage/sqlite.hpp> when it needs the concrete adapter.
class SQLiteStorage final : public Storage {
public:
  explicit SQLiteStorage(const std::filesystem::path &path);
  ~SQLiteStorage() override;
  SQLiteStorage(const SQLiteStorage &) = delete;
  SQLiteStorage &operator=(const SQLiteStorage &) = delete;
  void commit(const std::vector<Record> &) override;
  bool claim(const Record &, const std::vector<Record> &associated = {}) override;
  Json get(RecordKind, const std::string &) const override;
  std::vector<Json> list(RecordKind, const std::string &run_id = "", std::size_t limit = 1000,
                         std::size_t offset = 0) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace laso
