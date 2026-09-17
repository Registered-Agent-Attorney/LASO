#pragma once
#include <laso/core/async.hpp>
#include <laso/core/registry.hpp>
#include <cstdint>
#include <optional>

namespace laso {
enum class WorkerJobState {
  Created,
  Submitting,
  Queued,
  Running,
  Waiting,
  Completed,
  Failed,
  Cancelled,
  TimedOut,
  Unknown
};
NLOHMANN_JSON_SERIALIZE_ENUM(WorkerJobState, {{WorkerJobState::Created, "Created"},
                                              {WorkerJobState::Submitting, "Submitting"},
                                              {WorkerJobState::Queued, "Queued"},
                                              {WorkerJobState::Running, "Running"},
                                              {WorkerJobState::Waiting, "Waiting"},
                                              {WorkerJobState::Completed, "Completed"},
                                              {WorkerJobState::Failed, "Failed"},
                                              {WorkerJobState::Cancelled, "Cancelled"},
                                              {WorkerJobState::TimedOut, "TimedOut"},
                                              {WorkerJobState::Unknown, "Unknown"}})
bool worker_job_terminal(WorkerJobState state);
bool valid_worker_job_transition(WorkerJobState from, WorkerJobState to);

enum class WorkerFailureKind { None, Job, Transport, Budget };
NLOHMANN_JSON_SERIALIZE_ENUM(WorkerFailureKind, {{WorkerFailureKind::None, "none"},
                                                 {WorkerFailureKind::Job, "job"},
                                                 {WorkerFailureKind::Transport, "transport"},
                                                 {WorkerFailureKind::Budget, "budget"}})

// Usage is intentionally provider-neutral. Missing values mean that an
// adapter did not report that metric; they are not interpreted as zero.
struct WorkerUsage {
  std::optional<std::uint64_t> queue_duration_ms, wall_duration_ms;
  std::optional<std::uint64_t> input_tokens, output_tokens, total_tokens;
  std::optional<std::uint64_t> tool_calls, action_count;
  std::optional<double> cost_units;
  std::string provider, model, executor;
  Json metadata = Json::object();
};
void to_json(Json &, const WorkerUsage &);
void from_json(const Json &, WorkerUsage &);

class WorkerTransportError : public Error {
public:
  explicit WorkerTransportError(const std::string &message)
      : Error(ErrorCode::Plugin, message) {}
};

struct WorkerMetadata {
  std::string id, name, version, description, plugin, event_schema, event_source_id,
      status = "disabled";
  std::vector<std::string> capabilities;
  bool local = true, remote = false, healthy = false, enabled = false;
  bool supports_recovery = false, supports_cancellation = false;
};
void to_json(Json &, const WorkerMetadata &);
void from_json(const Json &, WorkerMetadata &);

struct WorkerRequest {
  std::string job_id, worker_id, capability, task_type, instructions, idempotency_key, deadline,
      run_id, node_id;
  unsigned attempt = 1;
  Json input = Json::object(), output_schema = Json::object(), metadata = Json::object();
  std::vector<std::string> artifact_ids;
};

struct WorkerSubmission {
  std::string external_job_id;
  WorkerJobState state = WorkerJobState::Queued;
  Json metadata = Json::object();
  WorkerUsage usage;
};

struct WorkerStatus {
  WorkerJobState state = WorkerJobState::Unknown;
  Json result = nullptr, metadata = Json::object();
  std::vector<Json> artifacts;
  std::string error;
  WorkerUsage usage;
};

struct WorkerJob {
  std::string id = uuid(), worker_id, run_id, node_id, idempotency_key, external_job_id,
              submitted_at = timestamp(), started_at, completed_at, error, cancellation_error;
  unsigned attempt = 1;
  WorkerJobState state = WorkerJobState::Created;
  WorkerFailureKind failure_kind = WorkerFailureKind::None;
  bool cancellation_requested = false, cancellation_acknowledged = false;
  Json request_metadata = Json::object(), result = Json::object(), result_metadata = Json::object();
  std::vector<Json> artifacts;
  WorkerUsage usage;
};
void to_json(Json &, const WorkerJob &);
void from_json(const Json &, WorkerJob &);

// The transport boundary is lifecycle- and job-operation-complete. Native
// plugins continue to implement WorkerAdapter below; supervised or remote
// implementations can implement this interface without changing WorkerNode.
class WorkerTransport {
public:
  virtual ~WorkerTransport() = default;
  virtual WorkerMetadata metadata() const = 0;
  virtual WorkerSubmission submit(const WorkerRequest &) = 0;
  virtual WorkerStatus status(const std::string &external_job_id) = 0;
  virtual WorkerStatus result(const std::string &external_job_id) = 0;
  virtual bool cancel(const std::string &external_job_id) = 0;
  virtual void start() = 0;
  virtual void stop() noexcept = 0;
};
// Source-compatible name for the original native adapter contract. Keeping it
// as an alias also lets existing Registry<WorkerAdapter>::add_batch callers
// use the transport registry without a conversion shim.
using WorkerAdapter = WorkerTransport;
using WorkerRegistry = Registry<WorkerTransport>;
} // namespace laso
