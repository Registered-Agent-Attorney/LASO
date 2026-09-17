#include <chrono>
#include <cstdlib>
#include <iostream>
#include <laso/workers/process_protocol.hpp>
#include <laso/workers/worker.hpp>
#include <map>
#include <string>
#include <thread>

using namespace laso;

namespace {
struct Job {
  std::string job_id;
  unsigned status_checks = 0;
  bool cancelled = false;
};

std::string option(int argc, char **argv, const std::string &name, std::string fallback) {
  for (int i = 1; i < argc; ++i) {
    const std::string value = argv[i];
    if (value == name && i + 1 < argc)
      return argv[++i];
    if (value.rfind(name + "=", 0) == 0)
      return value.substr(name.size() + 1);
  }
  return fallback;
}

void response(const Json &request, const Json &body,
              std::uint32_t protocol = process_protocol::version) {
  Json result = body;
  result["protocol_version"] = protocol;
  result["request_id"] = request.value("request_id", std::string{});
  std::cout << result.dump() << '\n' << std::flush;
}

Json usage(const std::string &mode) {
  return Json{{"input_tokens", 3},
              {"output_tokens", 2},
              {"total_tokens", 5},
              {"wall_duration_ms", 7},
              {"executor", "reference-worker"},
              {"cost_units", 0.5},
              {"metadata", Json{{"mode", mode}}}};
}

Json terminal_result(const std::string &mode) {
  Json result{{"ok", true}, {"worker", "process-reference"}, {"mode", mode}};
  if (mode == "environment") {
    result["parent_secret_inherited"] = std::getenv("LASO_PARENT_SECRET") != nullptr;
    result["explicit_override"] =
        std::getenv("LASO_REFERENCE") != nullptr ? std::getenv("LASO_REFERENCE") : "";
  }
  return result;
}
} // namespace

int main(int argc, char **argv) {
  const auto mode = option(argc, argv, "--mode", "success");
  const auto delay_ms = static_cast<unsigned>(std::stoul(option(argc, argv, "--delay-ms", "50")));
  std::map<std::string, Job> jobs;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.size() > process_protocol::max_frame_bytes)
      return 64;
    const auto request = Json::parse(line, nullptr, false);
    if (request.is_discarded() || !request.is_object())
      return 65;
    const auto operation = request.value("operation", std::string{});
    if (operation == "hello") {
      if (mode == "mismatch") {
        response(request, {{"ok", true}, {"metadata", Json::object()}}, 999);
        continue;
      }
      response(request, {{"ok", true},
                         {"metadata", Json{{"name", "process-reference"},
                                           {"version", "1"},
                                           {"description", "deterministic reference worker"},
                                           {"capabilities", Json::array({"deterministic"})},
                                           {"supports_recovery", true},
                                           {"supports_cancellation", true}}}});
      continue;
    }
    if (mode == "malformed") {
      std::cout << "this is not json\n" << std::flush;
      continue;
    }
    if (mode == "oversized") {
      std::cout << std::string(process_protocol::max_frame_bytes + 1, 'x') << '\n' << std::flush;
      continue;
    }
    if (mode == "truncated") {
      std::cout << "{\"protocol_version\":1" << std::flush;
      std::_Exit(74);
    }
    if (mode == "crash" || (mode == "exit-after-hello" && operation != "shutdown"))
      std::_Exit(73);
    if (mode == "hang" && operation != "shutdown")
      for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(60));
    if (operation == "shutdown") {
      response(request, {{"ok", true}, {"state", "Completed"}});
      return 0;
    }
    if (operation == "submit") {
      const auto job_id = request.value("job_id", std::string{});
      const auto external = "process-" + job_id;
      jobs[external] = {job_id, 0, false};
      if (mode == "failure") {
        response(request, {{"ok", true},
                           {"state", "Failed"},
                           {"external_job_id", external},
                           {"error", "reference worker declared failure"}});
      } else if (mode == "delay" || mode == "cancel") {
        response(request, {{"ok", true}, {"state", "Queued"}, {"external_job_id", external}});
      } else {
        if (mode == "delay-ms")
          std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        Json body{{"ok", true},
                  {"state", "Completed"},
                  {"external_job_id", external},
                  {"payload", terminal_result(mode)}};
        if (mode != "no-usage")
          body["usage"] = usage(mode);
        response(request, body);
      }
      continue;
    }
    const auto external = request.value("external_job_id", std::string{});
    auto found = jobs.find(external);
    if (operation == "cancel") {
      if (found != jobs.end())
        found->second.cancelled = true;
      response(request, {{"ok", true}, {"state", "Cancelled"}, {"acknowledged", true}});
      continue;
    }
    if (found == jobs.end()) {
      response(request, {{"ok", true}, {"state", "Unknown"}});
      continue;
    }
    if (found->second.cancelled) {
      response(request, {{"ok", true}, {"state", "Cancelled"}});
      continue;
    }
    ++found->second.status_checks;
    const bool done = mode != "delay" || found->second.status_checks > 1;
    Json body{{"ok", true},
              {"state", done ? "Completed" : "Running"},
              {"payload", done ? terminal_result(mode) : Json::object()}};
    if (done && mode != "no-usage")
      body["usage"] = usage(mode);
    response(request, body);
  }
  return 0;
}
