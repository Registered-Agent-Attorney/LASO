#include <algorithm>
#include <charconv>
#include <iomanip>
#include <laso/api/api.hpp>
#include <laso/pipeline/parser.hpp>
#include <limits>
#include <locale>
#include <regex>
#include <set>
#include <sstream>

namespace laso {
// NOLINTBEGIN(bugprone-exception-escape): this noexcept boundary converts all exceptions to HTTP
// responses.
ApiResponse Api::handle(const std::string &method, const std::string &target,
                        const std::string &body, const std::string &credential) noexcept {
  const auto started = std::chrono::steady_clock::now();
  auto response = [&]() -> ApiResponse {
    try {
      if (target.size() > 2048 || body.size() > max_document_bytes || credential.size() > 8192)
        return {413, {{"error", "Request exceeds size limit"}}};
      auto actor = identity_.authenticate(credential);
      if (!identity_.authorize({actor, method, target}))
        return {403, {{"error", "Access denied"}}};
      auto input = body.empty() ? Json::object() : Json::parse(body);
      if (!input.is_object())
        return {400, {{"error", "Request body must be a JSON object"}}};
      auto path = target;
      std::size_t limit = 50, offset = 0;
      std::uint64_t after = 0;
      bool has_after = false;
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
          else if (key == "offset" && number <= 100000000)
            offset = number;
          else if (key == "after" &&
                   number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            after = number;
            has_after = true;
          } else
            throw Error(ErrorCode::Validation, "Pagination limit exceeded or unknown parameter");
          if (end == std::string::npos)
            break;
          parameters.erase(0, end + 1);
        }
      }
      if (path.starts_with("/api/v1/operator/") &&
          (has_after || (has_query && (path == "/api/v1/operator/status" ||
                                       path == "/api/v1/operator/artifacts/integrity"))))
        throw Error(ErrorCode::Validation, "Unsupported operator pagination parameter");
      auto result = route(method, path, input, actor, limit, offset, after);
      const auto response_bytes =
          result.raw_body ? result.raw_body->size() : result.body.dump().size();
      if (response_bytes > std::size_t{4} * 1024 * 1024)
        return {413, {{"error", "Response exceeds limit; request a smaller page"}}};
      return result;
    } catch (const Error &error) {
      unsigned status = 400;
      if (error.code == ErrorCode::NotFound)
        status = 404;
      else if (error.code == ErrorCode::Conflict)
        status = 409;
      else if (error.code == ErrorCode::Capacity)
        status = 429;
      else if (error.code == ErrorCode::Policy)
        status = 403;
      else if (error.code == ErrorCode::Storage)
        status = 503;
      return {status, {{"error", status == 503 ? "Storage unavailable" : error.what()}}};
    } catch (const Json::exception &) {
      return {400, {{"error", "Malformed JSON request"}}};
    } catch (...) {
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
  return output.str();
}
// NOLINTEND(bugprone-exception-escape)
ApiResponse Api::route(const std::string &method, const std::string &target, const Json &body,
                       const Actor &actor, std::size_t limit, std::size_t offset,
                       std::uint64_t after) {
  static const std::regex operator_collection_route(
      "/api/v1/operator/(runs|node-work|worker-jobs|artifacts|sessions|approvals|"
      "worker-requests|attempts|providers|plugins|workers|instances|leases)");
  static const std::regex operator_session_turns_route(
      "/api/v1/operator/sessions/([A-Za-z0-9_.@-]{1,128})/turns");
  std::smatch operator_match;
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
                      "observability.prometheus_api_metrics"};
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
    if (method == "GET" && id.empty())
      return {200, service_.worker_jobs("", limit, offset)};
    if (method == "GET" && action.empty())
      return {200, service_.worker_job(id)};
    if (method == "POST" && action == "cancel") {
      service_.cancel_worker_job(id);
      return {202, service_.worker_job(id)};
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
  if (method == "GET" && id.empty())
    return {200, service_.list(kind, "", limit, offset)};
  if (method == "GET" && action.empty())
    return {200, collection == "runs" ? service_.run_view(id) : service_.get(kind, id)};
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
      return {200, service_.list(action == "events"     ? RecordKind::Event
                                 : action == "attempts" ? RecordKind::Attempt
                                                        : RecordKind::Message,
                                 id, limit, offset)};
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
