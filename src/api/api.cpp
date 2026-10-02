#include <algorithm>
#include <charconv>
#include <exception>
#include <iomanip>
#include <laso/api/api.hpp>
#include <laso/pipeline/parser.hpp>
#include <limits>
#include <locale>
#include <regex>
#include <set>
#include <sstream>

namespace laso {
namespace {
struct OperatorAction {
  std::string action;
  std::string target_type;
  std::string target_id;
};

std::optional<OperatorAction> operator_action_for(const std::string &method,
                                                  const std::string &path, const Actor &actor) {
  if (!actor.authenticated || (actor.role != "operator" && actor.role != "admin"))
    return std::nullopt;
  if (method == "POST") {
    static const std::regex maintenance(R"(^/api/v1/operator/maintenance/(drain|resume|enter)$)");
    std::smatch match;
    if (std::regex_match(path, match, maintenance))
      return OperatorAction{"maintenance." + match[1].str(), "instance", ""};
  }
  static const std::regex item_action(
      R"(^/api/v1/(sessions|worker-jobs|worker-requests|runs|approvals|event-sources|schedules|triggers|pipelines)/([A-Za-z0-9_.:@-]{1,128})/([a-z-]+)$)");
  std::smatch match;
  if (method == "POST" && std::regex_match(path, match, item_action)) {
    const auto collection = match[1].str();
    const auto id = match[2].str();
    const auto action = match[3].str();
    if (collection == "sessions" && action == "close")
      return OperatorAction{"session.close", "session", id};
    if (collection == "worker-jobs" && action == "cancel")
      return OperatorAction{"worker_job.cancel", "worker_job", id};
    if (collection == "worker-requests" &&
        (action == "respond" || action == "answer" || action == "approve" || action == "deny" ||
         action == "cancel"))
      return OperatorAction{"worker_request." + action, "worker_request", id};
    if (collection == "runs" && (action == "cancel" || action == "resume"))
      return OperatorAction{"run." + action, "run", id};
    if (collection == "approvals" && (action == "approve" || action == "reject"))
      return OperatorAction{"approval." + action, "approval", id};
    if (collection == "event-sources" && (action == "enable" || action == "disable"))
      return OperatorAction{"event_source." + action, "event_source", id};
    if ((collection == "schedules" || collection == "triggers") &&
        (action == "enable" || action == "disable"))
      return OperatorAction{(collection == "schedules" ? "schedule." : "trigger.") + action,
                            collection == "schedules" ? "schedule" : "trigger", id};
    if (collection == "pipelines" && action == "runs")
      return OperatorAction{"pipeline.run", "pipeline", id};
  }
  static const std::regex config_item(
      R"(^/api/v1/(schedules|triggers)/([A-Za-z0-9_.:@-]{1,128})$)");
  if ((method == "PATCH" || method == "DELETE") && std::regex_match(path, match, config_item)) {
    const auto collection = match[1].str();
    const auto type = collection == "schedules" ? "schedule" : "trigger";
    return OperatorAction{std::string(type) + (method == "PATCH" ? ".update" : ".delete"), type,
                          match[2].str()};
  }
  if (method == "POST" &&
      (path == "/api/v1/pipelines" || path == "/api/v1/schedules" || path == "/api/v1/triggers")) {
    const auto type = path == "/api/v1/pipelines"   ? "pipeline"
                      : path == "/api/v1/schedules" ? "schedule"
                                                    : "trigger";
    return OperatorAction{std::string(type) + ".create", type, ""};
  }
  return std::nullopt;
}

ApiResponse operator_audit_unavailable() {
  return {503, {{"error", "Operator audit is unavailable"}, {"error_code", "AUDIT_UNAVAILABLE"}}};
}
void redact_codex_session_fields(Json &value) {
  if (value.is_object()) {
    for (const auto *key :
         {"codex_session_id", "codex_turn_id", "codex_turn_started_at", "codex_turn_completed_at",
          "parent_codex_session_id", "parent_codex_turn_id", "session_id", "turn_id", "call_key",
          "project_dir"})
      value.erase(key);
    for (auto it = value.begin(); it != value.end(); ++it)
      redact_codex_session_fields(it.value());
  } else if (value.is_array()) {
    for (auto &entry : value)
      redact_codex_session_fields(entry);
  }
}
void redact_codex_external_handles(Json &value) {
  if (value.is_object()) {
    const auto external_id = value.find("external_job_id");
    if (external_id != value.end() && external_id->is_string() &&
        external_id->get_ref<const std::string &>().starts_with("codex:"))
      value.erase("external_job_id");
    for (auto it = value.begin(); it != value.end(); ++it)
      redact_codex_external_handles(it.value());
  } else if (value.is_array()) {
    for (auto &entry : value)
      redact_codex_external_handles(entry);
  }
}
bool is_codex_worker(const Json &worker_id) {
  return worker_id.is_string() && worker_id.get_ref<const std::string &>().starts_with("codex");
}
Json redact_worker_session_id(Json job) {
  if (!job.is_object())
    return job;
  const bool codex_worker = is_codex_worker(job.value("worker_id", Json{}));
  if (codex_worker) {
    if (job.contains("request_metadata"))
      redact_codex_session_fields(job["request_metadata"]);
    if (job.contains("result_metadata"))
      redact_codex_session_fields(job["result_metadata"]);
    if (job.contains("result"))
      redact_codex_session_fields(job["result"]);
  }
  redact_codex_external_handles(job);
  return job;
}
Json redact_worker_result_message(Json message) {
  if (!message.is_object() || !message.contains("metadata") || !message.at("metadata").is_object())
    return message;
  auto &metadata = message["metadata"];
  if (!is_codex_worker(metadata.value("worker_id", Json{})) || !metadata.contains("worker_job_id"))
    return message;
  if (metadata.contains("worker_result_metadata"))
    redact_codex_session_fields(metadata["worker_result_metadata"]);
  if (message.contains("payload"))
    redact_codex_session_fields(message["payload"]);
  redact_codex_external_handles(message);
  return message;
}
void redact_worker_result_messages(Json &value) {
  if (value.is_object()) {
    value = redact_worker_session_id(std::move(value));
    value = redact_worker_result_message(std::move(value));
    for (auto it = value.begin(); it != value.end(); ++it)
      redact_worker_result_messages(it.value());
  } else if (value.is_array()) {
    for (auto &entry : value)
      redact_worker_result_messages(entry);
  }
}
} // namespace

// NOLINTBEGIN(bugprone-exception-escape): this noexcept boundary converts all exceptions to HTTP
// responses.
ApiResponse Api::handle(const std::string &method, const std::string &target,
                        const std::string &body, const std::string &credential,
                        const std::string &principal, const std::string &role) noexcept {
  const auto started = std::chrono::steady_clock::now();
  std::optional<OperatorAction> pending_action;
  std::string pending_operation_id;
  Actor pending_actor;
  const auto finish_audit = [&](unsigned status) {
    if (!pending_action || pending_operation_id.empty())
      return true;
    const auto outcome = status >= 200 && status < 300 ? "succeeded"
                         : status >= 500               ? "failed"
                                                       : "rejected";
    try {
      service_.complete_operator_action(pending_actor, pending_operation_id, pending_action->action,
                                        pending_action->target_type, pending_action->target_id,
                                        outcome, status);
      pending_operation_id.clear();
      return true;
    } catch (...) {
      pending_operation_id.clear();
      return false;
    }
  };
  auto response = [&]() -> ApiResponse {
    try {
      if (target.size() > 2048 || body.size() > max_document_bytes || credential.size() > 8192)
        return {413, {{"error", "Request exceeds size limit"}}};
      auto actor = identity_.authenticate_request(credential, principal, role);
      pending_actor = actor;
      if (!identity_.authorize({actor, method, target}))
        return {403, {{"error", "Access denied"}}};
      auto input = body.empty() ? Json::object() : Json::parse(body);
      if (!input.is_object())
        return {400, {{"error", "Request body must be a JSON object"}}};
      auto path = target;
      std::size_t limit = 50, offset = 0;
      std::uint64_t after = 0;
      std::optional<std::uint64_t> before;
      bool has_after = false;
      bool has_offset = false;
      const auto query = path.find('?');
      const bool has_query = query != std::string::npos;
      if (has_query) {
        auto parameters = path.substr(query + 1);
        path.resize(query);
        std::set<std::string> seen;
        while (!parameters.empty()) {
          auto end = parameters.find('&');
          auto part = parameters.substr(0, end);
          auto equal = part.find('=');
          if (equal == std::string::npos)
            throw Error(ErrorCode::Validation, "Malformed pagination query");
          auto key = part.substr(0, equal), value = part.substr(equal + 1);
          std::uint64_t number = 0;
          auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
          if (!seen.insert(key).second || parsed.ec != std::errc{} ||
              parsed.ptr != value.data() + value.size())
            throw Error(ErrorCode::Validation, "Invalid pagination value");
          if (key == "limit" && number >= 1 && number <= 100)
            limit = number;
          else if (key == "offset" && number <= 100000000) {
            offset = number;
            has_offset = true;
          } else if (key == "after" && number <= static_cast<std::uint64_t>(
                                                     std::numeric_limits<std::int64_t>::max())) {
            after = number;
            has_after = true;
          } else if (key == "before" && path == "/api/v1/operator/audit" && number > 0 &&
                     number <=
                         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            before = number;
          } else
            throw Error(ErrorCode::Validation, "Pagination limit exceeded or unknown parameter");
          if (end == std::string::npos)
            break;
          parameters.erase(0, end + 1);
        }
      }
      if (path.starts_with("/api/v1/operator/") && has_after)
        throw Error(ErrorCode::Validation, "Unsupported operator pagination parameter");
      if (path != "/api/v1/operator/audit" && before)
        throw Error(ErrorCode::Validation, "Unsupported operator pagination parameter");
      if (path == "/api/v1/operator/audit" && has_offset)
        throw Error(ErrorCode::Validation, "Unsupported operator pagination parameter");
      if (has_query &&
          (path == "/api/v1/operator/status" || path == "/api/v1/operator/artifacts/integrity"))
        throw Error(ErrorCode::Validation, "Unsupported operator pagination parameter");
      pending_action = operator_action_for(method, path, actor);
      if (pending_action) {
        try {
          if (pending_action->target_type == "instance" && pending_action->target_id.empty())
            pending_action->target_id = service_.instance_id();
          pending_operation_id = service_.begin_operator_action(actor, pending_action->action,
                                                                pending_action->target_type,
                                                                pending_action->target_id);
        } catch (...) {
          return operator_audit_unavailable();
        }
      }
      auto result = route(method, path, input, actor, limit, offset, after, before);
      const auto response_bytes =
          result.raw_body ? result.raw_body->size() : result.body.dump().size();
      if (response_bytes > std::size_t{4} * 1024 * 1024) {
        if (!finish_audit(result.status))
          return operator_audit_unavailable();
        return {413, {{"error", "Response exceeds limit; request a smaller page"}}};
      }
      if (!finish_audit(result.status))
        return operator_audit_unavailable();
      return result;
    } catch (const Error &error) {
      unsigned status = 400;
      if (error.code == ErrorCode::NotFound)
        status = 404;
      else if (error.code == ErrorCode::Conflict)
        status = 409;
      else if (error.code == ErrorCode::Capacity)
        status = 429;
      else if (error.code == ErrorCode::Unavailable)
        status = 503;
      else if (error.code == ErrorCode::Policy)
        status = 403;
      else if (error.code == ErrorCode::Storage)
        status = 503;
      if (error.code == ErrorCode::Unavailable) {
        if (!finish_audit(503))
          return operator_audit_unavailable();
        return {503,
                {{"error", "Instance is not accepting new work"},
                 {"error_code", "INSTANCE_NOT_ACCEPTING_WORK"},
                 {"retry_after_seconds", 1}},
                "application/json",
                std::nullopt,
                1};
      }
      std::string code = error.code == ErrorCode::Validation ? "INVALID_REQUEST"
                         : error.code == ErrorCode::Conflict ? "CONFLICT"
                         : error.code == ErrorCode::Capacity ? "CAPACITY_EXHAUSTED"
                         : error.code == ErrorCode::Policy   ? "ACCESS_DENIED"
                         : error.code == ErrorCode::Storage  ? "STORAGE_UNAVAILABLE"
                                                             : "SERVICE_ERROR";
      if (!finish_audit(status))
        return operator_audit_unavailable();
      return {
          status,
          {{"error", status == 503 ? "Storage unavailable" : error.what()}, {"error_code", code}}};
    } catch (const Json::exception &) {
      if (!finish_audit(400))
        return operator_audit_unavailable();
      return {400, {{"error", "Malformed JSON request"}}};
    } catch (...) {
      if (!finish_audit(500))
        return operator_audit_unavailable();
      return {500, {{"error", "Internal service error"}}};
    }
  }();
  record_request(method, response.status, std::chrono::steady_clock::now() - started);
  return response;
}

void Api::record_request(std::string_view method, unsigned status,
                         std::chrono::steady_clock::duration duration) noexcept {
  static constexpr std::array<std::string_view, 6> methods{"GET",    "POST",  "PUT",
                                                           "DELETE", "PATCH", "OTHER"};
  const auto method_it = std::find(methods.begin(), methods.end(), method);
  const std::size_t method_index = method_it == methods.end()
                                       ? methods.size() - 1
                                       : static_cast<std::size_t>(method_it - methods.begin());
  const std::size_t status_index = status >= 200 && status < 300   ? 0
                                   : status >= 400 && status < 500 ? 1
                                   : status >= 500 && status < 600 ? 2
                                                                   : 3;
  request_counts_[method_index][status_index].fetch_add(1, std::memory_order_relaxed);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  const auto elapsed_ns = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
  request_duration_count_.fetch_add(1, std::memory_order_relaxed);
  request_duration_nanoseconds_.fetch_add(elapsed_ns, std::memory_order_relaxed);
  static constexpr std::array<std::uint64_t, 10> bounds_ns{
      5000000,   10000000,  25000000,   50000000,   100000000,
      250000000, 500000000, 1000000000, 2500000000, 5000000000};
  for (std::size_t index = 0; index < bounds_ns.size(); ++index)
    if (elapsed_ns <= bounds_ns[index])
      request_duration_buckets_[index].fetch_add(1, std::memory_order_relaxed);
}

std::string Api::prometheus_metrics() const {
  static constexpr std::array<std::string_view, 6> methods{"GET",    "POST",  "PUT",
                                                           "DELETE", "PATCH", "OTHER"};
  static constexpr std::array<std::string_view, 4> classes{"2xx", "4xx", "5xx", "other"};
  static constexpr std::array<std::string_view, 10> bucket_labels{
      "0.005", "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1", "2.5", "5"};
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "# HELP laso_api_responses_total Completed API responses by bounded method and status "
            "class.\n"
         << "# TYPE laso_api_responses_total counter\n";
  for (std::size_t method = 0; method < methods.size(); ++method)
    for (std::size_t status = 0; status < classes.size(); ++status)
      output << "laso_api_responses_total{method=\"" << methods[method] << "\",status_class=\""
             << classes[status] << "\"} "
             << request_counts_[method][status].load(std::memory_order_relaxed) << '\n';
  output << "# HELP laso_api_request_duration_seconds API request duration in seconds.\n"
         << "# TYPE laso_api_request_duration_seconds histogram\n";
  for (std::size_t index = 0; index < bucket_labels.size(); ++index)
    output << "laso_api_request_duration_seconds_bucket{le=\"" << bucket_labels[index] << "\"} "
           << request_duration_buckets_[index].load(std::memory_order_relaxed) << '\n';
  const auto count = request_duration_count_.load(std::memory_order_relaxed);
  output << "laso_api_request_duration_seconds_bucket{le=\"+Inf\"} " << count << '\n'
         << "laso_api_request_duration_seconds_count " << count << '\n'
         << std::setprecision(12) << "laso_api_request_duration_seconds_sum "
         << (static_cast<double>(request_duration_nanoseconds_.load(std::memory_order_relaxed)) /
             1000000000.0)
         << '\n';
  const auto maintenance = service_.maintenance_metrics();
  output << "# HELP laso_instance_maintenance_state Current instance state as a one-hot gauge.\n"
         << "# TYPE laso_instance_maintenance_state gauge\n";
  static constexpr std::array<std::string_view, 4> maintenance_states{"active", "draining",
                                                                      "drained", "maintenance"};
  for (const auto state : maintenance_states)
    output << "laso_instance_maintenance_state{state=\"" << state << "\"} "
           << (maintenance.at("state") == state ? 1 : 0) << '\n';
  output << "# HELP laso_instance_owned_runs Number of currently owned active runs.\n"
         << "# TYPE laso_instance_owned_runs gauge\n"
         << "laso_instance_owned_runs " << maintenance.at("owned_runs") << '\n'
         << "# HELP laso_instance_owned_nodes Number of currently owned active nodes.\n"
         << "# TYPE laso_instance_owned_nodes gauge\n"
         << "laso_instance_owned_nodes " << maintenance.at("owned_nodes") << '\n';
  return output.str();
}
// NOLINTEND(bugprone-exception-escape)
ApiResponse Api::route(const std::string &method, const std::string &target, const Json &body,
                       const Actor &actor, std::size_t limit, std::size_t offset,
                       std::uint64_t after, std::optional<std::uint64_t> before) {
  static const std::regex operator_collection_route(
      "/api/v1/operator/(runs|node-work|worker-jobs|artifacts|sessions|approvals|"
      "worker-requests|attempts|providers|plugins|workers|instances|leases)");
  static const std::regex operator_session_turns_route(
      "/api/v1/operator/sessions/([A-Za-z0-9_.@-]{1,128})/turns");
  std::smatch operator_match;
  static const std::regex maintenance_action_route(
      "/api/v1/operator/maintenance/(drain|resume|enter)");
  std::smatch maintenance_match;
  if (target == "/api/v1/operator/maintenance") {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    if (!actor.authenticated || (actor.role != "operator" && actor.role != "admin"))
      return {403, {{"error", "Operator access required"}}};
    return {200, service_.operator_maintenance()};
  }
  if (std::regex_match(target, maintenance_match, maintenance_action_route)) {
    if (method != "POST")
      return {405, {{"error", "Method not supported"}}};
    if (!actor.authenticated || (actor.role != "operator" && actor.role != "admin"))
      return {403, {{"error", "Operator access required"}}};
    if (body.size() != 1 || !body.contains("confirmed") || !body.at("confirmed").is_boolean() ||
        !body.at("confirmed").get<bool>())
      return {400,
              {{"error", "Explicit confirmation is required"},
               {"error_code", "CONFIRMATION_REQUIRED"}}};
    const auto action = maintenance_match[1].str();
    if (action == "drain")
      service_.request_drain(actor);
    else if (action == "resume")
      service_.resume_instance(actor);
    else
      service_.enter_maintenance(actor);
    return {200, service_.operator_maintenance()};
  }
  if (target == "/api/v1/operator/audit") {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    if (!actor.authenticated || (actor.role != "operator" && actor.role != "admin"))
      return {403, {{"error", "Operator access required"}}};
    return {200, service_.operator_audit_page(limit, before)};
  }
  if (target == "/api/v1/operator/status") {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    return {200, service_.operator_status()};
  }
  if (target == "/api/v1/operator/artifacts/integrity") {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    return {200, service_.operator_artifact_integrity()};
  }
  if (std::regex_match(target, operator_match, operator_session_turns_route)) {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    return {200, service_.operator_page("session-turns", limit, offset, operator_match[1].str())};
  }
  if (std::regex_match(target, operator_match, operator_collection_route)) {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    return {200, service_.operator_page(operator_match[1].str(), limit, offset)};
  }
  if (target.starts_with("/api/v1/operator/"))
    return {404, {{"error", "Endpoint not found"}}};

  if (target == "/api/v1/metrics") {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    return {200, Json::object(), "text/plain; version=0.0.4; charset=utf-8", prometheus_metrics()};
  }

  static const std::regex session_stream_pattern(
      "/api/v1/sessions/([A-Za-z0-9_.@-]{1,128})/events/stream");
  std::smatch stream_match;
  if (method == "GET" && std::regex_match(target, stream_match, session_stream_pattern)) {
    (void)service_.agent_session(stream_match[1].str());
    return {200, {{"status", "streaming"}}};
  }
  if (target == "/api/v1/health" || target == "/api/v1/health/live" ||
      target == "/api/v1/health/ready") {
    if (method != "GET")
      return {405, {{"error", "Method not supported"}}};
    if (target != "/api/v1/health/ready")
      return target == "/api/v1/health"
                 ? ApiResponse{200, {{"status", "ok"}, {"mode", "local-development"}}}
                 : ApiResponse{200, {{"status", "ok"}}};
    const auto readiness = service_.readiness();
    return {readiness.at("status") == "ready" ? 200U : 503U, readiness};
  }
  if (method == "GET" && target == "/api/v1/version") {
    Json capabilities{"sessions.durable",
                      "sessions.ordered_turns",
                      "sessions.sequential_execution",
                      "sessions.event_replay",
                      "sessions.sse",
                      "sessions.context_generations",
                      "sessions.run_context_snapshots",
                      "health.readiness",
                      "observability.prometheus_api_metrics",
                      "operator.coordinated_drain"};
    if (service_.context_reduction_available())
      capabilities.push_back("sessions.context_reduction");
    return {200,
            {{"version", version},
             {"pipeline_schema", 1},
             {"plugin_abi", 1},
             {"capabilities", std::move(capabilities)}}};
  }
  if (method == "GET" && target == "/api/v1/providers")
    return {200, service_.providers()};
  if (method == "GET" && target == "/api/v1/tools")
    return {200, service_.tools()};
  if (method == "GET" && target == "/api/v1/plugins")
    return {200, service_.plugins()};
  if (method == "GET" && target == "/api/v1/instances")
    return {200, service_.instances()};
  static const std::regex session_context_route(
      "/api/v1/sessions/([A-Za-z0-9_.@-]{1,128})/context(?:/generations)?");
  static const std::regex run_context_route("/api/v1/runs/([A-Za-z0-9_.@-]{1,128})/context");
  std::smatch context_match;
  if (std::regex_match(target, context_match, session_context_route)) {
    const auto session_id = context_match[1].str();
    const bool generations_path = target.ends_with("/context/generations");
    (void)service_.agent_session(session_id);
    const auto public_generation = [](Json generation) {
      generation.erase("payload");
      generation.erase("idempotency_key");
      return generation;
    };
    if (method == "GET" && !generations_path) {
      const auto generation = service_.latest_session_context_generation(session_id);
      return {
          200,
          {{"session_id", session_id},
           {"current_generation", generation ? public_generation(*generation) : Json(nullptr)}}};
    }
    if (method == "POST" && generations_path) {
      for (const auto *field : {"expected_generation", "through_turn_sequence", "idempotency_key",
                                "representation_kind", "representation_version", "payload"})
        if (!body.contains(field))
          return {400, {{"error", "Context generation request is incomplete"}}};
      auto generation = service_.create_session_context_generation(
          session_id, body.at("expected_generation").get<std::uint64_t>(),
          body.at("through_turn_sequence").get<std::uint64_t>(),
          body.at("idempotency_key").get<std::string>(),
          body.at("representation_kind").get<std::string>(),
          body.at("representation_version").get<std::string>(), body.at("payload"));
      return {201, public_generation(std::move(generation))};
    }
    return {405, {{"error", "Method not supported"}}};
  }
  if (method == "GET" && std::regex_match(target, context_match, run_context_route)) {
    const auto snapshot = service_.get(RecordKind::RunContextSnapshot, context_match[1].str());
    auto public_snapshot = snapshot;
    Json continuation_metadata = Json::array();
    for (const auto &continuation : snapshot.value("provider_continuations", Json::array()))
      continuation_metadata.push_back(
          {{"provider_id", continuation.value("provider_id", "")},
           {"provider_version", continuation.value("provider_version", "")}});
    public_snapshot["provider_continuations"] = std::move(continuation_metadata);
    return {200, public_snapshot};
  }
  std::smatch match;
  static const std::regex route_pattern(
      "/api/v1/"
      "(pipelines|runs|approvals|worker-requests|schedules|"
      "triggers|event-sources|workers|worker-jobs|sessions)(?:/"
      "([A-Za-z0-9_.@-]{1,128}))?(?:/"
      "(runs|cancel|resume|events|attempts|messages|approve|"
      "reject|respond|answer|deny|enable|disable|turns|close|context))?");
  if (!std::regex_match(target, match, route_pattern))
    return {404, {{"error", "Endpoint not found"}}};
  auto collection = match[1].str(), id = match[2].str(), action = match[3].str();
  if (collection == "sessions") {
    auto public_turn = [](Json turn) {
      turn.erase("dispatch_owner");
      turn.erase("dispatch_fencing_token");
      turn.erase("dispatch_expires_at");
      turn.erase("dispatch_attempt");
      return turn;
    };
    if (method == "POST" && id.empty()) {
      const auto session = service_.create_session(body.at("pipeline_id").get<std::string>());
      auto result = Json(session);
      result.erase("next_sequence");
      return {201, result};
    }
    if (method == "GET" && id.empty()) {
      auto sessions = service_.list(RecordKind::AgentSession, "", limit, offset);
      for (auto &session : sessions) {
        session.erase("next_sequence");
        session.erase("dispatch_generation");
      }
      return {200, sessions};
    }
    if (method == "GET" && !id.empty() && action.empty()) {
      auto result = Json(service_.agent_session(id));
      result.erase("next_sequence");
      result.erase("dispatch_generation");
      return {200, result};
    }
    if (method == "POST" && !id.empty() && action == "turns") {
      if (!body.contains("idempotency_key") || !body.contains("input"))
        return {400, {{"error", "Session turn requires idempotency_key and input"}}};
      return {202, public_turn(service_.submit_session_turn(
                       id, body.at("idempotency_key").get<std::string>(), body.at("input")))};
    }
    if (method == "GET" && !id.empty() && action == "turns") {
      (void)service_.agent_session(id);
      auto turns = service_.list(RecordKind::SessionTurn, id, limit, offset);
      for (auto &turn : turns)
        turn = public_turn(std::move(turn));
      return {200, turns};
    }
    if (method == "GET" && !id.empty() && action == "events")
      return {200, service_.session_events(id, after, limit)};
    if (method == "POST" && !id.empty() && action == "close") {
      service_.close_session(id);
      auto result = Json(service_.agent_session(id));
      result.erase("next_sequence");
      return {202, result};
    }
    return {405, {{"error", "Method not supported"}}};
  }
  if (collection == "event-sources") {
    if (method == "GET" && id.empty())
      return {200, service_.event_sources()};
    if (method == "GET" && action.empty())
      return {200, service_.event_source(id)};
    if (method == "POST" && !id.empty() && (action == "enable" || action == "disable")) {
      service_.set_event_source_enabled(id, action == "enable");
      return {202, service_.event_source(id)};
    }
    return {405, {{"error", "Method not supported"}}};
  }
  if (collection == "workers") {
    if (method == "GET" && id.empty())
      return {200, service_.workers()};
    if (method == "GET" && action.empty())
      return {200, service_.worker(id)};
    return {405, {{"error", "Method not supported"}}};
  }
  if (collection == "worker-jobs") {
    if (method == "GET" && id.empty()) {
      auto jobs = service_.worker_jobs("", limit, offset);
      for (auto &job : jobs)
        job = redact_worker_session_id(std::move(job));
      return {200, jobs};
    }
    if (method == "GET" && action.empty())
      return {200, redact_worker_session_id(service_.worker_job(id))};
    if (method == "POST" && action == "cancel") {
      service_.cancel_worker_job(id);
      return {202, redact_worker_session_id(service_.worker_job(id))};
    }
    return {405, {{"error", "Method not supported"}}};
  }
  if (collection == "worker-requests") {
    if (method == "GET" && id.empty())
      return {200, service_.worker_interactions("", limit, offset)};
    if (method == "GET" && action.empty())
      return {200, service_.worker_interaction(id)};
    if (method == "POST" && !id.empty() &&
        (action == "respond" || action == "answer" || action == "approve" || action == "deny" ||
         action == "cancel")) {
      const auto state = action == "approve" ? WorkerInteractionState::Approved
                         : action == "answer" || action == "respond"
                             ? WorkerInteractionState::Answered
                         : action == "cancel" ? WorkerInteractionState::Cancelled
                                              : WorkerInteractionState::Denied;
      service_.resolve_worker_interaction(id, state, body.value("payload", Json::object()),
                                          actor.id, body.value("reason", std::string{}));
      return {202, service_.worker_interaction(id)};
    }
    return {405, {{"error", "Method not supported"}}};
  }
  auto kind = collection == "pipelines"   ? RecordKind::Pipeline
              : collection == "runs"      ? RecordKind::Run
              : collection == "approvals" ? RecordKind::Approval
              : collection == "schedules" ? RecordKind::Schedule
                                          : RecordKind::Trigger;
  if (method == "GET" && id.empty()) {
    auto records = service_.list(kind, "", limit, offset);
    if (collection == "runs")
      for (auto &run : records)
        redact_worker_result_messages(run);
    return {200, records};
  }
  if (method == "GET" && action.empty()) {
    if (collection == "runs") {
      auto run = service_.run_view(id);
      redact_worker_result_messages(run);
      return {200, run};
    }
    return {200, service_.get(kind, id)};
  }
  if (method == "POST" && collection == "pipelines" && id.empty())
    return {201, service_.register_pipeline(body.at("yaml").get<std::string>())};
  if (method == "POST" && collection == "schedules" && id.empty())
    return {201, service_.create_schedule(body)};
  if (method == "POST" && collection == "triggers" && id.empty())
    return {201, service_.create_trigger(body)};
  if (method == "PATCH" && collection == "schedules" && !id.empty() && action.empty())
    return {200, service_.update_schedule(id, body)};
  if (method == "PATCH" && collection == "triggers" && !id.empty() && action.empty())
    return {200, service_.update_trigger(id, body)};
  if (method == "DELETE" && collection == "schedules" && !id.empty() && action.empty()) {
    service_.delete_schedule(id);
    return {202, {{"id", id}, {"deleted", true}}};
  }
  if (method == "DELETE" && collection == "triggers" && !id.empty() && action.empty()) {
    service_.delete_trigger(id);
    return {202, {{"id", id}, {"deleted", true}}};
  }
  if (method == "POST" && collection == "schedules" && !id.empty() &&
      (action == "enable" || action == "disable")) {
    service_.set_schedule_enabled(id, action == "enable");
    return {202, service_.get(RecordKind::Schedule, id)};
  }
  if (method == "POST" && collection == "triggers" && !id.empty() &&
      (action == "enable" || action == "disable")) {
    service_.set_trigger_enabled(id, action == "enable");
    return {202, service_.get(RecordKind::Trigger, id)};
  }
  if (method == "POST" && collection == "pipelines" && action == "runs")
    return {202,
            {{"id", service_.start(id, body.value("input", Json::object()), actor.id, false,
                                   Json::object(), body.value("metadata", Json::object()))}}};
  if (collection == "runs" && !id.empty()) {
    if (method == "GET" && (action == "events" || action == "attempts" || action == "messages")) {
      (void)service_.get(RecordKind::Run, id);
      auto records = service_.list(action == "events"     ? RecordKind::Event
                                   : action == "attempts" ? RecordKind::Attempt
                                                          : RecordKind::Message,
                                   id, limit, offset);
      if (action == "messages")
        for (auto &message : records)
          message = redact_worker_result_message(std::move(message));
      if (action == "attempts")
        for (auto &attempt : records)
          redact_codex_external_handles(attempt);
      return {200, records};
    }
    if (method == "POST" && action == "cancel") {
      service_.runtime().cancel(id);
      return {202, {{"id", id}, {"action", "cancellation requested"}}};
    }
    if (method == "POST" && action == "resume") {
      service_.runtime().resume(id);
      return {202, {{"id", id}}};
    }
  }
  if (method == "POST" && collection == "approvals" &&
      (action == "approve" || action == "reject")) {
    service_.runtime().decide(id, action == "approve", actor.id,
                              body.value("comment", std::string{}));
    return {202, service_.get(RecordKind::Approval, id)};
  }
  return {405, {{"error", "Method not supported"}}};
}
} // namespace laso
