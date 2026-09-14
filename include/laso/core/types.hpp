#pragma once
#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace laso {
using Json = nlohmann::json;
using Milliseconds = std::chrono::milliseconds;
inline constexpr auto version = "0.1.0";
std::string uuid();
std::string timestamp();
enum class ErrorCode {
  Configuration,
  Validation,
  Plugin,
  Storage,
  Provider,
  Tool,
  Policy,
  Execution,
  Timeout,
  Cancellation,
  NotFound,
  Conflict,
  Capacity
};
class Error : public std::runtime_error {
public:
  Error(ErrorCode code, const std::string &safe_message, Json details = Json::object())
      : std::runtime_error(safe_message), code(code), details(std::move(details)) {}
  ErrorCode code;
  Json details;
};
enum class RunState {
  Queued,
  Starting,
  Running,
  WaitingTool,
  WaitingModel,
  WaitingApproval,
  Retrying,
  Paused,
  Completed,
  Failed,
  Cancelled,
  TimedOut
};
NLOHMANN_JSON_SERIALIZE_ENUM(RunState, {{RunState::Queued, "Queued"},
                                        {RunState::Starting, "Starting"},
                                        {RunState::Running, "Running"},
                                        {RunState::WaitingTool, "WaitingTool"},
                                        {RunState::WaitingModel, "WaitingModel"},
                                        {RunState::WaitingApproval, "WaitingApproval"},
                                        {RunState::Retrying, "Retrying"},
                                        {RunState::Paused, "Paused"},
                                        {RunState::Completed, "Completed"},
                                        {RunState::Failed, "Failed"},
                                        {RunState::Cancelled, "Cancelled"},
                                        {RunState::TimedOut, "TimedOut"}})
bool terminal(RunState state);
bool valid_transition(RunState from, RunState to);
enum class NodeState { Running, Completed, Failed, WaitingApproval, Cancelled, TimedOut };
NLOHMANN_JSON_SERIALIZE_ENUM(NodeState, {{NodeState::Running, "Running"},
                                         {NodeState::Completed, "Completed"},
                                         {NodeState::Failed, "Failed"},
                                         {NodeState::WaitingApproval, "WaitingApproval"},
                                         {NodeState::Cancelled, "Cancelled"},
                                         {NodeState::TimedOut, "TimedOut"}})
struct ProvenanceRecord {
  std::string node, tool, model, provider, artifact, parent_message, validation, time;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProvenanceRecord, node, tool, model, provider, artifact,
                                   parent_message, validation, time)
struct Message {
  std::string id = uuid(), run_id, pipeline_id, node_id, type = "laso.data", time = timestamp();
  Json payload = Json::object(), metadata = Json::object();
  std::vector<ProvenanceRecord> provenance;
  // A missing confidence is serialized as JSON null.
  std::optional<double> confidence;
};
void to_json(Json &json, const Message &message);
void from_json(const Json &json, Message &message);
struct RetryPolicy {
  unsigned max_attempts = 1;
  Milliseconds delay{0};
};
struct TimeoutPolicy {
  Milliseconds timeout{30000};
};
struct NodeDefinition {
  std::string id, type, binding, prompt, field, condition, reason, join, input_schema,
      output_schema, schema;
  Json value = nullptr;
  RetryPolicy retry;
  TimeoutPolicy timeout;
  unsigned max_iterations = 0;
};
struct EdgeDefinition {
  std::string from, to, condition;
  unsigned max_iterations = 0;
};
struct PipelineDefinition {
  std::string name, source;
  unsigned schema_version = 1, version = 1, max_steps = 1000;
  TimeoutPolicy timeout{Milliseconds{300000}};
  std::map<std::string, NodeDefinition> nodes;
  std::vector<EdgeDefinition> edges;
};
struct BranchFrame {
  std::string group, join;
  unsigned width = 0, index = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BranchFrame, group, join, width, index)
struct ExecutionToken {
  std::string node_id;
  Message message;
  std::vector<BranchFrame> frames;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExecutionToken, node_id, message, frames)
struct JoinCheckpoint {
  std::map<unsigned, Message> messages;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(JoinCheckpoint, messages)
struct Run {
  std::string id = uuid(), pipeline_id, definition, active_node = "input", created_at = timestamp(),
              updated_at = created_at, actor = "local", error;
  RunState state = RunState::Queued;
  bool cancellation_requested = false;
  Message message;
  std::map<std::string, unsigned> edge_visits, node_visits;
  std::vector<BranchFrame> frames;
  std::vector<ExecutionToken> ready;
  std::map<std::string, JoinCheckpoint> joins;
  // A nested pipeline returns to this durable parent; empty for root runs.
  std::string parent_id, child_id, prepared_join;
  std::string provider, model, tool, plugin;
  unsigned steps = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Run, id, pipeline_id, definition, active_node, created_at,
                                   updated_at, actor, error, state, cancellation_requested, message,
                                   edge_visits, node_visits, frames, ready, joins, parent_id,
                                   child_id, prepared_join, provider, model, tool, plugin, steps)
struct NodeExecution {
  std::string id = uuid(), run_id, node_id, started_at = timestamp(), finished_at, error;
  unsigned attempt = 1;
  NodeState state = NodeState::Running;
  double duration_ms = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NodeExecution, id, run_id, node_id, started_at, finished_at,
                                   error, attempt, state, duration_ms)
struct Approval {
  std::string id = uuid(), run_id, node_id, reason, action, created_at = timestamp(),
              decision = "pending", decided_at, actor, comment;
  unsigned visit = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Approval, id, run_id, node_id, reason, action, created_at,
                                   decision, decided_at, actor, comment, visit)
struct Event {
  std::string id = uuid(), run_id, pipeline_id, node_id, type, time = timestamp();
  Json metadata = Json::object();
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Event, id, run_id, pipeline_id, node_id, type, time, metadata)
struct Artifact {
  std::string id = uuid(), run_id, node_id, name, media_type, location, created_at = timestamp();
  Json metadata = Json::object();
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Artifact, id, run_id, node_id, name, media_type, location,
                                   created_at, metadata)
} // namespace laso
