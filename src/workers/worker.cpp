#include <laso/workers/worker.hpp>

namespace laso {
bool worker_job_terminal(WorkerJobState state) {
  return state == WorkerJobState::Completed || state == WorkerJobState::Failed ||
         state == WorkerJobState::Cancelled || state == WorkerJobState::TimedOut;
}

bool valid_worker_job_transition(WorkerJobState from, WorkerJobState to) {
  if (worker_job_terminal(from))
    return false;
  switch (from) {
  case WorkerJobState::Created:
    return to == WorkerJobState::Submitting || to == WorkerJobState::Failed ||
           to == WorkerJobState::Cancelled || to == WorkerJobState::Unknown;
  case WorkerJobState::Submitting:
    return to == WorkerJobState::Queued || to == WorkerJobState::Running ||
           to == WorkerJobState::Completed || to == WorkerJobState::Failed ||
           to == WorkerJobState::Cancelled || to == WorkerJobState::TimedOut ||
           to == WorkerJobState::Unknown;
  case WorkerJobState::Queued:
    return to == WorkerJobState::Running || to == WorkerJobState::Waiting ||
           to == WorkerJobState::Completed || to == WorkerJobState::Failed ||
           to == WorkerJobState::Cancelled || to == WorkerJobState::TimedOut ||
           to == WorkerJobState::Unknown;
  case WorkerJobState::Running:
  case WorkerJobState::Waiting:
  case WorkerJobState::Unknown:
    return to == WorkerJobState::Running || to == WorkerJobState::Waiting ||
           to == WorkerJobState::Completed || to == WorkerJobState::Failed ||
           to == WorkerJobState::Cancelled || to == WorkerJobState::TimedOut ||
           to == WorkerJobState::Unknown;
  default:
    return false;
  }
}

void to_json(Json &j, const WorkerMetadata &m) {
  j = {{"id", m.id},
       {"name", m.name},
       {"version", m.version},
       {"description", m.description},
       {"plugin", m.plugin},
       {"event_schema", m.event_schema},
       {"event_source_id", m.event_source_id},
       {"status", m.status},
       {"capabilities", m.capabilities},
       {"local", m.local},
       {"remote", m.remote},
       {"healthy", m.healthy},
       {"enabled", m.enabled},
       {"supports_recovery", m.supports_recovery},
       {"supports_cancellation", m.supports_cancellation}};
}

void from_json(const Json &j, WorkerMetadata &m) {
  m.id = j.value("id", std::string{});
  m.name = j.value("name", std::string{});
  m.version = j.value("version", std::string{});
  m.description = j.value("description", std::string{});
  m.plugin = j.value("plugin", std::string{});
  m.event_schema = j.value("event_schema", std::string{});
  m.event_source_id = j.value("event_source_id", std::string{});
  m.status = j.value("status", std::string{"disabled"});
  m.capabilities = j.value("capabilities", std::vector<std::string>{});
  m.local = j.value("local", true);
  m.remote = j.value("remote", false);
  m.healthy = j.value("healthy", false);
  m.enabled = j.value("enabled", false);
  m.supports_recovery = j.value("supports_recovery", false);
  m.supports_cancellation = j.value("supports_cancellation", false);
}

void to_json(Json &j, const WorkerJob &job) {
  j = {{"id", job.id},
       {"worker_id", job.worker_id},
       {"run_id", job.run_id},
       {"node_id", job.node_id},
       {"idempotency_key", job.idempotency_key},
       {"external_job_id", job.external_job_id},
       {"attempt", job.attempt},
       {"status", job.state},
       {"submitted_at", job.submitted_at},
       {"started_at", job.started_at},
       {"completed_at", job.completed_at},
       {"error", job.error},
       {"cancellation_error", job.cancellation_error},
       {"cancellation_requested", job.cancellation_requested},
       {"cancellation_acknowledged", job.cancellation_acknowledged},
       {"request_metadata", job.request_metadata},
       {"result", job.result},
       {"result_metadata", job.result_metadata},
       {"artifacts", job.artifacts}};
}

void from_json(const Json &j, WorkerJob &job) {
  j.at("id").get_to(job.id);
  j.at("worker_id").get_to(job.worker_id);
  j.at("run_id").get_to(job.run_id);
  j.at("node_id").get_to(job.node_id);
  job.idempotency_key = j.value("idempotency_key", std::string{});
  job.external_job_id = j.value("external_job_id", std::string{});
  job.attempt = j.value("attempt", 1U);
  j.at("status").get_to(job.state);
  job.submitted_at = j.value("submitted_at", std::string{});
  job.started_at = j.value("started_at", std::string{});
  job.completed_at = j.value("completed_at", std::string{});
  job.error = j.value("error", std::string{});
  job.cancellation_error = j.value("cancellation_error", std::string{});
  job.cancellation_requested = j.value("cancellation_requested", false);
  job.cancellation_acknowledged = j.value("cancellation_acknowledged", false);
  job.request_metadata = j.value("request_metadata", Json::object());
  job.result = j.value("result", Json::object());
  job.result_metadata = j.value("result_metadata", Json::object());
  job.artifacts = j.value("artifacts", std::vector<Json>{});
}
} // namespace laso
