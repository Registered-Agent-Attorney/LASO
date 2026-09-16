#pragma once
#include <laso/core/types.hpp>
#include <memory>
#include <string>
#include <vector>

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
} // namespace laso
