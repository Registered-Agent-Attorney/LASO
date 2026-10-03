// Optional Codex adapter. LASO speaks the versioned worker protocol; this
// executable speaks Codex's structured local app-server protocol. Keeping the
// two protocols here prevents Codex types and policies from entering Core.
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <laso/workers/process_protocol.hpp>
#include <laso/workers/worker.hpp>
#include <limits>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <thread>
#include <unistd.h>

using namespace laso;

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_codex_line = 4 * 1024 * 1024;
constexpr std::size_t max_codex_stderr = 64 * 1024;
constexpr std::size_t max_text = 256 * 1024;
constexpr std::size_t max_actions = 128;

std::string validate_direct_luna_catalog(const std::string &path) {
  if (path.empty())
    return {};
  const std::filesystem::path configured(path);
  if (!configured.is_absolute())
    throw WorkerTransportError("Codex model catalog path must be absolute");
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(configured, ec);
  if (ec || !std::filesystem::is_regular_file(canonical))
    throw WorkerTransportError("Codex model catalog file is unavailable");
  const auto canonical_string = canonical.string();
  if (canonical_string.find_first_of("\r\n\"\\") != std::string::npos)
    throw WorkerTransportError("Codex model catalog path contains unsupported characters");
  std::ifstream file(canonical, std::ios::binary);
  if (!file)
    throw WorkerTransportError("Codex model catalog file cannot be read");
  const auto catalog = Json::parse(file, nullptr, false);
  if (catalog.is_discarded() || !catalog.is_object() || !catalog.contains("models") ||
      !catalog.at("models").is_array() || catalog.at("models").size() != 1)
    throw WorkerTransportError("Codex direct-tool catalog must contain only GPT-6 Luna");
  const auto &model = catalog.at("models").front();
  if (!model.is_object())
    throw WorkerTransportError("Codex direct-tool catalog model entry is invalid");
  const auto levels = model.value("supported_reasoning_levels", Json::array());
  const auto experimental_tools = model.value("experimental_supported_tools", Json::array());
  if (model.value("slug", std::string{}) != "gpt-6-luna" ||
      model.value("tool_mode", std::string{}) != "direct" ||
      model.value("shell_type", std::string{}) != "disabled" ||
      model.value("multi_agent_version", std::string{}) != "disabled" ||
      (model.contains("apply_patch_tool_type") && !model.at("apply_patch_tool_type").is_null()) ||
      model.value("supports_search_tool", true) || !experimental_tools.is_array() ||
      !experimental_tools.empty() ||
      model.value("default_reasoning_level", std::string{}) != "high" ||
      !levels.is_array() || levels.size() != 1 || !levels.front().is_object() ||
      levels.front().value("effort", std::string{}) != "high")
    throw WorkerTransportError("Codex direct-tool catalog must enforce GPT-6 Luna High and disable other native tools");
  return canonical_string;
}

bool arm_parent_death_signal() noexcept {
#ifdef __linux__
  const auto parent = ::getppid();
  if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent)
    return false;
#endif
  return true;
}

bool process_group_alive(pid_t process_group) noexcept {
  if (process_group <= 0)
    return false;
  if (::kill(-process_group, 0) == 0)
    return true;
  return errno == EPERM;
}

void terminate_process_group(pid_t process_group) noexcept {
  if (process_group <= 0)
    return;
  if (::kill(-process_group, SIGTERM) < 0 && errno != ESRCH)
    (void)::kill(process_group, SIGTERM);
  const auto graceful_deadline = Clock::now() + std::chrono::milliseconds(500);
  while (process_group_alive(process_group) && Clock::now() < graceful_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!process_group_alive(process_group))
    return;
  if (::kill(-process_group, SIGKILL) < 0 && errno != ESRCH)
    (void)::kill(process_group, SIGKILL);
}

void close_fd(int &fd) {
  if (fd >= 0)
    ::close(fd);
  fd = -1;
}

int remaining_ms(Clock::time_point deadline) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  if (remaining.count() <= 0)
    return 0;
  // poll() accepts an int timeout.  Do not cap this to one minute: a bounded
  // poll interval is not the provider deadline, and treating it as one used
  // to turn a quiet but valid Codex turn into a false timeout at 60 seconds.
  return static_cast<int>(std::min<std::int64_t>(
      remaining.count(), static_cast<std::int64_t>(std::numeric_limits<int>::max())));
}

std::string option(int argc, char **argv, const std::string &name, std::string fallback = {}) {
  for (int i = 1; i < argc; ++i) {
    const std::string value = argv[i];
    if (value == name && i + 1 < argc)
      return argv[++i];
    if (value.rfind(name + "=", 0) == 0)
      return value.substr(name.size() + 1);
  }
  return fallback;
}

std::vector<std::string> options(int argc, char **argv, const std::string &name) {
  std::vector<std::string> result;
  for (int i = 1; i < argc; ++i) {
    const std::string value = argv[i];
    if (value == name && i + 1 < argc)
      result.push_back(argv[++i]);
    else if (value.rfind(name + "=", 0) == 0)
      result.push_back(value.substr(name.size() + 1));
  }
  return result;
}

bool valid_worker_id(const std::string &value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
         });
}

bool within(const std::filesystem::path &path, const std::filesystem::path &root) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(path, root, ec);
  const auto text = relative.generic_string();
  return !ec && relative != ".." && text.rfind("../", 0) != 0;
}

class CodexFailure final : public std::runtime_error {
public:
  explicit CodexFailure(const std::string &message) : std::runtime_error(message) {}
};

struct TurnStartObservation {
  std::string turn_id;
  std::string observed_at;
};

class CodexProcess {
public:
  CodexProcess(std::string executable, std::uint64_t timeout_ms,
               std::string model_catalog_json = {})
      : executable_(std::move(executable)), timeout_ms_(timeout_ms),
        model_catalog_json_(std::move(model_catalog_json)) {}
  ~CodexProcess() { stop(); }
  CodexProcess(const CodexProcess &) = delete;
  CodexProcess &operator=(const CodexProcess &) = delete;

  void start() {
    if (pid_ > 0)
      return;
    int child_in[2] = {-1, -1}, child_out[2] = {-1, -1}, child_err[2] = {-1, -1};
    if (::pipe(child_in) < 0 || ::pipe(child_out) < 0 || ::pipe(child_err) < 0) {
      close_fd(child_in[0]);
      close_fd(child_in[1]);
      close_fd(child_out[0]);
      close_fd(child_out[1]);
      close_fd(child_err[0]);
      close_fd(child_err[1]);
      throw WorkerTransportError("Unable to create Codex app-server pipes");
    }
    auto close_all = [&]() noexcept {
      close_fd(child_in[0]);
      close_fd(child_in[1]);
      close_fd(child_out[0]);
      close_fd(child_out[1]);
      close_fd(child_err[0]);
      close_fd(child_err[1]);
    };
    pid_ = ::fork();
    if (pid_ < 0) {
      close_all();
      throw WorkerTransportError("Unable to start Codex app-server");
    }
    if (pid_ == 0) {
      if (!arm_parent_death_signal())
        _exit(125);
      (void)::setpgid(0, 0);
      if (::dup2(child_in[0], STDIN_FILENO) < 0 || ::dup2(child_out[1], STDOUT_FILENO) < 0 ||
          ::dup2(child_err[1], STDERR_FILENO) < 0)
        _exit(126);
      close_all();
      const std::string listen = "stdio://";
      std::vector<std::string> arguments{executable_, "app-server"};
      if (!model_catalog_json_.empty()) {
        arguments.push_back("-c");
        arguments.push_back("model_catalog_json=\"" + model_catalog_json_ + "\"");
      }
      arguments.push_back("--listen");
      arguments.push_back(listen);
      std::vector<char *> argv;
      argv.reserve(arguments.size() + 1);
      for (auto &argument : arguments)
        argv.push_back(argument.data());
      argv.push_back(nullptr);
      if (executable_.find('/') != std::string::npos)
        ::execv(executable_.c_str(), argv.data());
      else
        ::execvp(executable_.c_str(), argv.data());
      _exit(127);
    }
    close_fd(child_in[0]);
    close_fd(child_out[1]);
    close_fd(child_err[1]);
    input_fd_ = child_in[1];
    output_fd_ = child_out[0];
    error_fd_ = child_err[0];
    for (const auto fd : {input_fd_, output_fd_, error_fd_}) {
      const auto flags = ::fcntl(fd, F_GETFL, 0);
      if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        stop();
        throw WorkerTransportError("Unable to configure Codex app-server pipes");
      }
    }
    (void)::setpgid(pid_, pid_);
  }

  void stop() noexcept {
    if (pid_ > 0) {
      const auto process_group = pid_;
      terminate_process_group(process_group);
      const auto deadline = Clock::now() + std::chrono::milliseconds(500);
      int status = 0;
      while (Clock::now() < deadline) {
        const auto result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (::waitpid(pid_, &status, WNOHANG) == 0)
        (void)::waitpid(pid_, &status, 0);
    }
    pid_ = -1;
    close_fd(input_fd_);
    close_fd(output_fd_);
    close_fd(error_fd_);
  }

  bool alive() const {
    if (pid_ <= 0)
      return false;
    int status = 0;
    return ::waitpid(pid_, &status, WNOHANG) == 0;
  }

  void send(const Json &message, Clock::time_point deadline) {
    const auto wire = message.dump();
    if (wire.size() > max_codex_line)
      throw WorkerTransportError("Codex message exceeds the limit");
    write_all(wire + '\n', deadline);
  }

  Json receive(Clock::time_point deadline) {
    std::string line;
    char buffer[4096];
    for (;;) {
      const auto newline = output_.find('\n');
      if (newline != std::string::npos) {
        if (newline > max_codex_line)
          throw WorkerTransportError("Codex message exceeds the limit");
        line = output_.substr(0, newline);
        output_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        const auto parsed = Json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
          throw WorkerTransportError("Codex emitted malformed JSON");
        return parsed;
      }
      if (output_.size() > max_codex_line)
        throw WorkerTransportError("Codex message exceeds the limit");
      pollfd descriptors[2] = {{output_fd_, POLLIN, 0}, {error_fd_, POLLIN, 0}};
      const auto count = ::poll(descriptors, error_fd_ >= 0 ? 2 : 1, remaining_ms(deadline));
      if (count == 0)
        throw WorkerTransportError("Codex request timed out");
      if (count < 0) {
        if (errno == EINTR)
          continue;
        throw WorkerTransportError("Unable to read Codex response");
      }
      drain_stderr(descriptors[1].revents);
      if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        const auto received = ::read(output_fd_, buffer, sizeof(buffer));
        if (received > 0)
          output_.append(buffer, static_cast<std::size_t>(received));
        else if (received == 0)
          throw WorkerTransportError("Codex app-server exited unexpectedly");
        else if (errno != EAGAIN && errno != EINTR)
          throw WorkerTransportError("Unable to read Codex response");
      }
      if (!alive())
        throw WorkerTransportError("Codex app-server exited unexpectedly");
    }
  }

private:
  std::string executable_;
  std::uint64_t timeout_ms_;
  pid_t pid_ = -1;
  int input_fd_ = -1, output_fd_ = -1, error_fd_ = -1;
  std::string output_, stderr_;
  std::string model_catalog_json_;

  void write_all(const std::string &wire, Clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < wire.size()) {
      pollfd descriptor{input_fd_, POLLOUT, 0};
      if (::poll(&descriptor, 1, remaining_ms(deadline)) <= 0)
        throw WorkerTransportError("Codex request timed out");
      if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        throw WorkerTransportError("Codex app-server closed its input");
      const auto count = ::write(input_fd_, wire.data() + offset, wire.size() - offset);
      if (count > 0)
        offset += static_cast<std::size_t>(count);
      else if (count < 0 && errno != EAGAIN && errno != EINTR)
        throw WorkerTransportError("Unable to write Codex request");
    }
  }

  void drain_stderr(short events) {
    if (error_fd_ < 0 || !(events & (POLLIN | POLLHUP | POLLERR)))
      return;
    char buffer[4096];
    const auto count = ::read(error_fd_, buffer, sizeof(buffer));
    if (count > 0) {
      if (stderr_.size() > max_codex_stderr - static_cast<std::size_t>(count))
        throw WorkerTransportError("Codex stderr exceeds the limit");
      stderr_.append(buffer, static_cast<std::size_t>(count));
    } else if (count == 0) {
      close_fd(error_fd_);
    } else if (errno != EAGAIN && errno != EINTR) {
      close_fd(error_fd_);
    }
  }
};

class CodexAdapter {
public:
  CodexAdapter(std::string worker_id, std::string executable,
               std::vector<std::filesystem::path> roots, std::uint64_t timeout_ms,
               std::string model_catalog_json)
      : executable_(std::move(executable)), timeout_ms_(timeout_ms),
        model_catalog_json_(validate_direct_luna_catalog(model_catalog_json)),
        process_(executable_, timeout_ms_, model_catalog_json_), worker_id_(std::move(worker_id)) {
    if (!valid_worker_id(worker_id_))
      throw WorkerTransportError("Codex worker id is invalid");
    for (const auto &root : roots) {
      std::error_code ec;
      const auto canonical = std::filesystem::canonical(root, ec);
      if (ec || !std::filesystem::is_directory(canonical))
        throw WorkerTransportError("Codex allowed root is not a directory");
      roots_.push_back(canonical);
    }
    if (roots_.empty())
      throw WorkerTransportError("At least one Codex allowed root is required");
  }

  void start() {
    process_.start();
    const auto response = call(
        "initialize",
        Json{{"clientInfo", Json{{"name", "laso-codex-worker"}, {"version", "0.1"}}},
             {"capabilities", Json{{"experimentalApi", true}}}},
        {});
    if (!response.is_object())
      throw WorkerTransportError("Codex initialize response is invalid");
    Json initialized{{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", Json::object()}};
    process_.send(initialized, deadline());
    healthy_ = true;
  }

  WorkerMetadata metadata() const {
    WorkerMetadata result;
    result.id = worker_id_;
    result.name = worker_id_ == "codex" ? "Codex coding worker"
                                        : "Codex coding worker (" + worker_id_ + ")";
    result.version = "app-server";
    result.description = "Optional supervised Codex coding worker";
    result.plugin = "process";
    result.capabilities = {"coding", "coding-agent", "session-continuation", "session-context"};
    result.local = true;
    result.status = healthy_ ? "healthy" : "stopped";
    result.healthy = healthy_;
    result.enabled = true;
    result.supports_recovery = true;
    result.supports_cancellation = true;
    return result;
  }

  WorkerSubmission submit(const Json &request) {
    if (!healthy_)
      throw WorkerTransportError("Codex worker is unavailable");
    const auto payload = request.value("payload", Json::object());
    const auto metadata = payload.value("metadata", Json::object());
    const auto required_tool = metadata.value("required_tool", std::string{});
    if (!required_tool.empty() && required_tool != "laso.browser_status")
      throw CodexFailure("LASO worker request requires an unsupported dynamic tool");
    if (required_tool == "laso.browser_status" && model_catalog_json_.empty())
      throw CodexFailure("LASO browser.status requires the pinned GPT-6 Luna High direct-tool catalog");
    const auto directory = canonical_project(metadata.value("project_dir", std::string{}));
    const bool durable_session = payload.value("durable_session", false);
    durable_session_id_ = durable_session ? payload.value("durable_session_id", std::string{})
                                         : std::string{};
    if (durable_session && durable_session_id_.empty())
      throw CodexFailure("Durable Codex request has no LASO session identity");
    if (durable_session && required_tool == "laso.browser_status")
      throw CodexFailure("Required LASO browser status calls need a fresh non-durable Codex turn");
    const auto session_context = payload.value("session_context", Json::object());
    const auto context_generation =
        session_context.is_object() ? session_context.value("generation_id", std::string{})
                                    : std::string{};
    const auto requested_session = metadata.value("codex_session_id", std::string{});
    if (required_tool == "laso.browser_status") {
      reset_thread();
      start_thread(directory, metadata);
    } else if (durable_session) {
      std::string previous_session;
      std::string previous_generation;
      if (payload.contains("continuation") && !payload.at("continuation").is_null()) {
        const auto &continuation = payload.at("continuation");
        if (!continuation.is_object() ||
            continuation.value("provider_id", std::string{}) != "codex" ||
            continuation.value("provider_version", std::string{}) != "app-server")
          throw CodexFailure("Stored Codex continuation is incompatible");
        const auto state = Json::parse(continuation.value("state", std::string{}), nullptr, false);
        if (state.is_discarded() || !state.is_object() ||
            state.value("format", std::string{}) != "laso-codex-session-v1")
          throw CodexFailure("Stored Codex continuation is invalid");
        previous_session = state.value("thread_id", std::string{});
        previous_generation = state.value("context_generation_id", std::string{});
        if (previous_session.empty())
          throw CodexFailure("Stored Codex continuation has no thread");
      }

      if (!previous_session.empty() && previous_generation == context_generation) {
        if (session_id_ != previous_session || project_dir_ != directory)
          resume_thread(previous_session, directory);
      } else {
        reset_thread();
        start_thread(directory, metadata);
      }
    } else {
      if (requested_session.empty() && !session_id_.empty())
        reset_thread();
      else if (!requested_session.empty() && requested_session != session_id_)
        resume_thread(requested_session, directory);
      if (session_id_.empty())
        start_thread(directory, metadata);
      else if (directory != project_dir_) {
        if (!requested_session.empty())
          throw CodexFailure("Codex session belongs to a different project directory");
        // Distributed attempts use fresh, owner-scoped staging directories.
        // Start a new provider thread for a new directory instead of reusing a
        // session bound to the previous attempt's workspace.
        reset_thread();
        start_thread(directory, metadata);
      }
    }

    summary_.clear();
    actions_ = Json::array();
    usage_ = WorkerUsage{};
    auto instructions = payload.value("instructions", std::string{});
    if (instructions.size() > max_text)
      throw WorkerTransportError("Codex instructions exceed the limit");
    if (durable_session && !session_context.is_null() && !session_context.empty() &&
        (payload.value("continuation", Json::object()).is_null() ||
         previous_context_generation(payload.value("continuation", Json::object())) !=
             context_generation)) {
      Json bounded_context = session_context;
      bounded_context.erase("generation_id");
      instructions += "\n\nLASO derived session context (bounded state and recent completed turns):\n" +
                      bounded_context.dump();
    }
    Json input{{"type", "text"}, {"text", instructions}};
    if (payload.contains("input") && !payload.at("input").is_null())
      input["text"] = instructions + "\n\nStructured input:\n" + payload.at("input").dump();
    if (input.at("text").get<std::string>().size() > max_text)
      throw WorkerTransportError("Codex session input exceeds the limit");
    active_job_id_ = request.value("job_id", std::string{});
    active_deadline_ = payload.value("deadline", std::string{});
    TurnStartObservation start_observation;
    const auto turn_response = call("turn/start", Json{{"threadId", session_id_},
                                                         {"input", Json::array({input})},
                                                         {"cwd", project_dir_}},
                                    request.value("job_id", std::string{}), &start_observation);
    const auto turn_id = turn_response.value("turn", Json::object()).value("id", std::string{});
    if (turn_id.empty())
      throw WorkerTransportError("Codex turn/start response has no turn id");
    active_turn_id_ = turn_id;
    std::string turn_started_at;
    if (start_observation.turn_id == turn_id)
      turn_started_at = start_observation.observed_at;
    std::string turn_completed_at;
    Json completed;
    while (true) {
      const auto message = process_.receive(deadline());
      if (handle_server_request(message, request.value("job_id", std::string{})))
        continue;
      if (message.value("method", std::string{}) == "turn/started") {
        const auto params = message.value("params", Json::object());
        const auto started_turn = params.value("turn", Json::object());
        if (params.value("threadId", std::string{}) == session_id_ &&
            started_turn.value("id", std::string{}) == turn_id && turn_started_at.empty())
          turn_started_at = timestamp();
        continue;
      }
      if (message.value("method", std::string{}) == "thread/tokenUsage/updated") {
        record_usage(message.value("params", Json::object()));
        continue;
      }
      if (message.value("method", std::string{}) == "item/completed") {
        record_item(message.value("params", Json::object()));
        continue;
      }
      if (message.value("method", std::string{}) == "turn/completed") {
        const auto params = message.value("params", Json::object());
        const auto completed_turn = params.value("turn", Json::object());
        if (params.value("threadId", std::string{}) == session_id_ &&
            completed_turn.value("id", std::string{}) == turn_id) {
          completed = params;
          turn_completed_at = timestamp();
          break;
        }
        continue;
      }
      if (message.contains("id") && message.value("id", 0) == 0)
        throw WorkerTransportError("Codex emitted an unrelated response");
    }
    active_turn_id_.clear();
    active_job_id_.clear();
    active_deadline_.clear();
    const auto turn = completed.value("turn", Json::object());
    const auto status = turn.value("status", std::string{});
    if (status == "failed" || status == "interrupted")
      throw CodexFailure(turn.value("error", Json::object()).value("message", "Codex turn failed"));
    WorkerSubmission result;
    result.external_job_id = durable_session ? request.value("job_id", std::string{})
                                             : "codex:" + session_id_;
    result.state = WorkerJobState::Completed;
    result.usage = usage_;
    result.metadata = {{"model", model_}, {"provider", provider_},
                       {"reasoningEffort", reasoning_effort_}};
    result.metadata["codex_turn_id"] = turn_id;
    result.metadata["codex_session_id"] = session_id_;
    if (!turn_started_at.empty())
      result.metadata["codex_turn_started_at"] = turn_started_at;
    if (!turn_completed_at.empty())
      result.metadata["codex_turn_completed_at"] = turn_completed_at;
    result.result = {{"summary", summary_}};
    if (!durable_session) {
      result.metadata["codex_session_id"] = session_id_;
      result.metadata["project_dir"] = project_dir_;
      result.result["session_id"] = session_id_;
      result.result["project_dir"] = project_dir_;
      result.result["actions"] = actions_;
    } else {
      result.continuation = OpaqueProviderContinuation{
          "codex", "app-server",
          Json{{"format", "laso-codex-session-v1"},
               {"thread_id", session_id_},
               {"context_generation_id", context_generation}}
              .dump()};
    }
    if (!model_.empty())
      result.result["model"] = model_;
    if (!provider_.empty())
      result.result["provider"] = provider_;
    if (turn.contains("durationMs") && turn.at("durationMs").is_number_unsigned())
      result.usage.wall_duration_ms = turn.at("durationMs").get<std::uint64_t>();
    return result;
  }

  WorkerStatus status(const std::string &) const {
    WorkerStatus result;
    result.state = WorkerJobState::Unknown;
    result.error = "Codex app-server does not expose a safe external-turn lookup through this adapter";
    return result;
  }

  WorkerStatus result(const std::string &external) const { return status(external); }

  bool cancel(const std::string &external_job_id) {
    if (session_id_.empty() || active_turn_id_.empty())
      return false;
    if (!active_job_id_.empty() && external_job_id != active_job_id_ &&
        external_job_id != "codex:" + session_id_ && external_job_id != session_id_)
      return false;
    const auto response = call("turn/interrupt", Json{{"threadId", session_id_}}, {});
    return response.is_object();
  }

  void stop() noexcept {
    healthy_ = false;
    process_.stop();
  }

private:
  std::string executable_;
  std::uint64_t timeout_ms_;
  std::vector<std::filesystem::path> roots_;
  std::string model_catalog_json_;
  CodexProcess process_;
  bool healthy_ = false;
  std::uint64_t rpc_id_ = 0, interaction_id_ = 0;
  std::string worker_id_, session_id_, durable_session_id_, project_dir_, active_turn_id_,
      active_job_id_, active_deadline_, model_, provider_, reasoning_effort_, summary_;
  WorkerUsage usage_;
  Json actions_ = Json::array();

  Clock::time_point deadline() const {
    return Clock::now() + std::chrono::milliseconds(timeout_ms_);
  }

  std::string previous_context_generation(const Json &continuation) const {
    if (!continuation.is_object())
      return {};
    const auto state = Json::parse(continuation.value("state", std::string{}), nullptr, false);
    return state.is_object() ? state.value("context_generation_id", std::string{}) : std::string{};
  }

  void reset_thread() {
    session_id_.clear();
    project_dir_.clear();
    active_turn_id_.clear();
    active_job_id_.clear();
    model_.clear();
    provider_.clear();
    reasoning_effort_.clear();
  }

  std::filesystem::path canonical_project(const std::string &value) const {
    if (value.empty() && roots_.size() == 1)
      return roots_.front();
    if (value.empty())
      throw CodexFailure("Codex request requires metadata.project_dir");
    std::error_code ec;
    const auto project = std::filesystem::canonical(value, ec);
    if (ec || !std::filesystem::is_directory(project))
      throw CodexFailure("Codex project directory is invalid");
    for (const auto &root : roots_)
      if (within(project, root))
        return project;
    throw CodexFailure("Codex project directory is outside an allowed root");
  }

  Json call(const std::string &method, const Json &params, const std::string &job_id,
            TurnStartObservation *turn_start = nullptr) {
    const auto id = ++rpc_id_;
    process_.send(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}},
                  deadline());
    for (;;) {
      const auto message = process_.receive(deadline());
      if (handle_server_request(message, job_id))
        continue;
      if (message.contains("id") && message.at("id") == id) {
        if (message.contains("error"))
          throw CodexFailure(message.at("error").value("message", "Codex request failed"));
        return message.value("result", Json::object());
      }
      if (message.value("method", std::string{}) == "thread/tokenUsage/updated") {
        record_usage(message.value("params", Json::object()));
        continue;
      }
      if (message.value("method", std::string{}) == "item/completed") {
        record_item(message.value("params", Json::object()));
        continue;
      }
      if (message.value("method", std::string{}) == "turn/started") {
        const auto notification = message.value("params", Json::object());
        const auto turn = notification.value("turn", Json::object());
        if (turn_start && notification.value("threadId", std::string{}) == session_id_ &&
            !turn.value("id", std::string{}).empty()) {
          turn_start->turn_id = turn.value("id", std::string{});
          turn_start->observed_at = timestamp();
        }
        continue;
      }
      if (!message.contains("id") && message.contains("method"))
        continue;
      throw WorkerTransportError("Codex response id does not match the request");
    }
  }

  bool handle_server_request(const Json &message, const std::string &job_id) {
    if (!message.contains("method") || !message.contains("id"))
      return false;
    const auto method = message.value("method", std::string{});
    if (method == "item/tool/call")
      return handle_dynamic_tool_call(message, job_id);
    WorkerInteractionType type;
    if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval")
      type = method.find("fileChange") != std::string::npos ? WorkerInteractionType::Approval
                                                               : WorkerInteractionType::Permission;
    else if (method == "item/permissions/requestApproval")
      type = WorkerInteractionType::Permission;
    else if (method == "item/tool/requestUserInput")
      type = WorkerInteractionType::Question;
    else {
      process_.send(Json{{"jsonrpc", "2.0"}, {"id", message.at("id")},
                         {"error", Json{{"code", -32601}, {"message", "Unsupported Codex request"}}}},
                    deadline());
      return true;
    }
    const auto params = message.value("params", Json::object());
    const auto request_id = "codex-interaction-" + std::to_string(++interaction_id_);
    Json worker_request{{"protocol_version", process_protocol::version},
                        {"message_type", "worker_request"},
                        {"request_id", request_id},
                        {"worker_job_id", job_id},
                        {"worker_id", worker_id_},
                        {"external_job_id", durable_session_id_.empty()
                                                ? "codex:" + session_id_
                                                : job_id},
                        {"session_id", durable_session_id_.empty() ? session_id_
                                                                   : durable_session_id_},
                        {"request_type", type},
                        {"title", method},
                        {"summary", params.value("reason", "Codex requests a decision")},
                        {"payload", params},
                        {"created_at", timestamp()},
                        {"risk", "unknown"},
                        {"category", "coding-worker"}};
    if (worker_request.dump().size() > process_protocol::max_frame_bytes)
      throw WorkerTransportError("Codex interaction request exceeds the limit");
    std::cout << worker_request.dump() << '\n' << std::flush;
    const auto answer = receive_worker_response(request_id);
    Json response;
    if (type == WorkerInteractionType::Question) {
      response = answer.payload;
      if (!response.is_object())
        response = Json::object();
    } else {
      const auto accepted = answer.state == WorkerInteractionState::Approved;
      response["decision"] = accepted ? "accept" :
          (answer.state == WorkerInteractionState::Cancelled || answer.state == WorkerInteractionState::Expired
               ? "cancel"
               : "decline");
    }
    process_.send(Json{{"jsonrpc", "2.0"}, {"id", message.at("id")}, {"result", response}}, deadline());
    return true;
  }

  bool handle_dynamic_tool_call(const Json &message, const std::string &job_id) {
    const auto params = message.value("params", Json::object());
    const auto request_id = params.value("callId", std::string{});
    const auto thread_id = params.value("threadId", std::string{});
    const auto turn_id = params.value("turnId", std::string{});
    if (job_id.empty() || job_id != active_job_id_ || active_turn_id_.empty() ||
        request_id.empty() || request_id.size() > 512 || thread_id.empty() ||
        thread_id != session_id_ || turn_id.empty() || turn_id.size() > 512 ||
        turn_id != active_turn_id_) {
      process_.send(Json{{"jsonrpc", "2.0"},
                         {"id", message.at("id")},
                         {"error", Json{{"code", -32602},
                                        {"message", "Invalid LASO tool call context"}}}},
                    deadline());
      return true;
    }
    Json worker_request{{"protocol_version", process_protocol::version},
                        {"message_type", "worker_tool_call"},
                        {"request_id", request_id},
                        {"worker_job_id", job_id},
                        {"worker_id", worker_id_},
                        {"external_job_id", durable_session_id_.empty()
                                                ? "codex:" + session_id_
                                                : job_id},
                        {"session_id", session_id_},
                        {"turn_id", turn_id},
                        {"deadline", active_deadline_},
                        {"namespace", params.value("namespace", std::string{})},
                        {"tool", params.value("tool", std::string{})},
                        {"arguments", params.value("arguments", Json::object())}};
    if (worker_request.dump().size() > process_protocol::max_frame_bytes)
      throw WorkerTransportError("Codex tool call exceeds the worker protocol limit");
    std::cout << worker_request.dump() << '\n' << std::flush;
    const auto answer = receive_worker_tool_response(request_id);
    const auto text = answer.success ? answer.result.dump() : answer.error;
    process_.send(Json{{"jsonrpc", "2.0"},
                       {"id", message.at("id")},
                       {"result", Json{{"success", answer.success},
                                        {"contentItems", Json::array({Json{{"type", "inputText"},
                                                                         {"text", text}}})}}}},
                  deadline());
    return true;
  }

  WorkerToolCallResponse receive_worker_tool_response(const std::string &request_id) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    if (::poll(&descriptor, 1, static_cast<int>(std::min<std::uint64_t>(timeout_ms_, 60000))) <= 0)
      throw WorkerTransportError("LASO tool call response timed out");
    std::string line;
    if (!std::getline(std::cin, line) || line.size() > process_protocol::max_frame_bytes)
      throw WorkerTransportError("LASO tool call response was truncated");
    const auto parsed = Json::parse(line, nullptr, false);
    if (parsed.is_discarded() ||
        parsed.value("message_type", std::string{}) != "worker_tool_response" ||
        parsed.value("request_id", std::string{}) != request_id)
      throw WorkerTransportError("LASO tool call response is invalid");
    try {
      return parsed.get<WorkerToolCallResponse>();
    } catch (...) {
      throw WorkerTransportError("LASO tool call response has invalid fields");
    }
  }

  WorkerInteractionResponse receive_worker_response(const std::string &request_id) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    if (::poll(&descriptor, 1, static_cast<int>(std::min<std::uint64_t>(timeout_ms_, 60000))) <= 0)
      throw WorkerTransportError("LASO interaction response timed out");
    std::string line;
    if (!std::getline(std::cin, line) || line.size() > process_protocol::max_frame_bytes)
      throw WorkerTransportError("LASO interaction response was truncated");
    const auto parsed = Json::parse(line, nullptr, false);
    if (parsed.is_discarded() || parsed.value("message_type", std::string{}) != "worker_response" ||
        parsed.value("request_id", std::string{}) != request_id)
      throw WorkerTransportError("LASO interaction response is invalid");
    try {
      return parsed.get<WorkerInteractionResponse>();
    } catch (...) {
      throw WorkerTransportError("LASO interaction response has invalid fields");
    }
  }

  void start_thread(const std::filesystem::path &directory, const Json &metadata) {
    Json params{{"cwd", directory.string()}, {"approvalPolicy", "on-request"},
                {"sandbox", "workspace-write"}, {"ephemeral", false}};
    if (metadata.value("required_tool", std::string{}) == "laso.browser_status") {
      params["config"] = Json{{"features.code_mode", false},
                              {"features.code_mode_only", false},
                              {"features.multi_agent", false},
                              {"features.multi_agent_v2", false},
                              {"features.sleep_tool", false},
                              {"features.standalone_web_search", false},
                              {"features.apps", false},
                              {"features.plugins", false},
                              {"features.view_image", false},
                              {"features.shell_tool", false},
                              {"features.unified_exec", false},
                              {"agents.enabled", false},
                              {"web_search", "disabled"},
                              {"tools.update_plan.enabled", false},
                              {"tools.experimental_request_user_input.enabled", false}};
      params["developerInstructions"] =
          "LASO required-tool restriction: this task has exactly one task-specific tool, "
          "laso.browser_status({}). Invoke it exactly once before answering, then use its "
          "returned result verbatim. Do not use built-in exec or shell, Code Mode, file tools, "
          "native Codex collaboration/subagent tools, other tools, or browser actions. "
          "If the LASO tool is unavailable or fails, stop and "
          "report that failure; never substitute or claim a result.";
      params["dynamicTools"] = Json::array({browser_status_tool_spec()});
    }
    if (metadata.contains("model") && metadata.at("model").is_string())
      params["model"] = metadata.at("model");
    const auto response = call("thread/start", params, {});
    const auto thread = response.value("thread", Json::object());
    session_id_ = thread.value("id", std::string{});
    if (session_id_.empty())
      throw WorkerTransportError("Codex thread/start response has no session id");
    project_dir_ = directory.string();
    model_ = response.value("model", thread.value("model", std::string{}));
    provider_ = response.value("modelProvider", thread.value("modelProvider", std::string{}));
    reasoning_effort_ =
        response.value("reasoningEffort", thread.value("reasoningEffort", std::string{}));
    if (provider_ != "openai" || model_ != "gpt-6-luna" || reasoning_effort_ != "high")
      throw CodexFailure("LASO requires openai/gpt-6-luna with high reasoning");
  }

  static Json browser_status_tool_spec() {
    return Json{{"type", "namespace"},
                {"name", "laso"},
                {"description",
                 "Required acceptance tool: call exactly once with {} and use the returned "
                 "status verbatim. Read-only LASO Computer status."},
                {"tools", Json::array({Json{{"type", "function"},
                                           {"name", "browser_status"},
                                           {"description",
                                            "You must call this exactly once with empty JSON "
                                            "arguments {} for the LASO full-system acceptance. "
                                            "Do not claim it is unavailable; wait for and use "
                                            "its returned result verbatim. Read-only status "
                                            "from the configured Windows Computer."},
                                           {"deferLoading", false},
                                           {"inputSchema", Json{{"type", "object"},
                                                                 {"properties", Json::object()},
                                                                 {"required", Json::array()},
                                                                 {"additionalProperties", false}}}}})}};
  }

  void resume_thread(const std::string &session, const std::filesystem::path &directory) {
    const auto response =
        call("thread/resume", Json{{"threadId", session}, {"cwd", directory.string()},
                                    {"excludeTurns", true}},
             {});
    const auto thread = response.value("thread", Json::object());
    session_id_ = thread.value("id", session);
    project_dir_ = thread.value("cwd", std::string{});
    if (session_id_.empty() || project_dir_.empty())
      throw CodexFailure("Codex session could not be safely resumed");
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(project_dir_, ec);
    const auto permitted = !ec && std::any_of(roots_.begin(), roots_.end(), [&](const auto &root) {
      return within(canonical, root);
    });
    if (!permitted)
      throw CodexFailure("Resumed Codex session is outside an allowed root");
    model_ = response.value("model", thread.value("model", std::string{}));
    provider_ = response.value("modelProvider", thread.value("modelProvider", std::string{}));
    reasoning_effort_ =
        response.value("reasoningEffort", thread.value("reasoningEffort", std::string{}));
    if (provider_ != "openai" || model_ != "gpt-6-luna" || reasoning_effort_ != "high")
      throw CodexFailure("LASO requires openai/gpt-6-luna with high reasoning");
  }

  void record_usage(const Json &params) {
    const auto usage = params.value("tokenUsage", Json::object()).value("last", Json::object());
    if (usage.contains("inputTokens") && usage.at("inputTokens").is_number_unsigned())
      usage_.input_tokens = usage.at("inputTokens").get<std::uint64_t>();
    if (usage.contains("outputTokens") && usage.at("outputTokens").is_number_unsigned())
      usage_.output_tokens = usage.at("outputTokens").get<std::uint64_t>();
    if (usage.contains("totalTokens") && usage.at("totalTokens").is_number_unsigned())
      usage_.total_tokens = usage.at("totalTokens").get<std::uint64_t>();
    usage_.executor = "codex";
    usage_.provider = provider_;
    usage_.model = model_;
  }

  void record_item(const Json &params) {
    const auto item = params.value("item", Json::object());
    const auto type = item.value("type", std::string{});
    if (type == "agentMessage")
      summary_ = item.value("text", summary_);
    else if (type == "commandExecution") {
      if (actions_.size() < max_actions)
        actions_.push_back(Json{{"command", item.value("command", std::string{})},
                                {"status", item.value("status", std::string{})},
                                {"exit_code", item.value("exitCode", Json())}});
      if (!usage_.action_count)
        usage_.action_count = 0;
      ++*usage_.action_count;
    }
  }
};

void response(const Json &request, const Json &body) {
  auto result = body;
  result["protocol_version"] = process_protocol::version;
  result["request_id"] = request.value("request_id", std::string{});
  std::cout << result.dump() << '\n' << std::flush;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (!arm_parent_death_signal())
      return 125;
    const auto worker_id = option(argc, argv, "--worker-id", "codex");
    const auto codex = option(argc, argv, "--codex", "codex");
    const auto model_catalog_json = option(argc, argv, "--codex-model-catalog");
    auto roots = options(argc, argv, "--allowed-root");
    const auto timeout = std::stoull(option(argc, argv, "--timeout-ms", "60000"));
    CodexAdapter adapter(worker_id, codex, {roots.begin(), roots.end()}, timeout,
                         model_catalog_json);
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.size() > process_protocol::max_frame_bytes)
        return 64;
      const auto request = Json::parse(line, nullptr, false);
      if (request.is_discarded() || !request.is_object())
        return 65;
      const auto operation = request.value("operation", std::string{});
      try {
        if (operation == "hello") {
          adapter.start();
          response(request, { {"ok", true}, {"metadata", Json(adapter.metadata())} });
        } else if (operation == "submit") {
          auto result = adapter.submit(request);
          Json body{{"ok", true}, {"state", result.state}, {"external_job_id", result.external_job_id},
                    {"payload", result.result}, {"metadata", result.metadata}, {"usage", result.usage}};
          if (result.continuation)
            body["continuation"] = {{"provider_id", result.continuation->provider_id},
                                    {"provider_version", result.continuation->provider_version},
                                    {"state", result.continuation->state}};
          response(request, body);
        } else if (operation == "status" || operation == "result") {
          const auto result = operation == "status" ? adapter.status(request.value("external_job_id", ""))
                                                      : adapter.result(request.value("external_job_id", ""));
          response(request, {{"ok", true}, {"state", result.state}, {"error", result.error}});
        } else if (operation == "cancel") {
          response(request, {{"ok", true}, {"acknowledged", adapter.cancel(request.value("external_job_id", ""))}});
        } else if (operation == "shutdown") {
          adapter.stop();
          response(request, {{"ok", true}, {"state", "Completed"}});
          return 0;
        } else {
          response(request, {{"ok", false}, {"error", "Unsupported operation"}});
        }
      } catch (const CodexFailure &error) {
        const auto job_id = request.value("job_id", std::string{"rejected"});
        response(request, {{"ok", true}, {"state", "Failed"},
                           {"external_job_id", "codex-rejected-" + job_id},
                           {"error", error.what()}});
      }
    }
    adapter.stop();
    return 0;
  } catch (const WorkerTransportError &error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 71;
  }
}
