#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <laso/workers/process_transport.hpp>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace laso {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_id_bytes = 512;
constexpr std::size_t max_error_bytes = 512;

void close_fd(int &fd) {
  if (fd >= 0)
    ::close(fd);
  fd = -1;
}

void nonblocking(int fd) {
  const auto flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    throw WorkerTransportError("Unable to configure worker process pipe");
}

int remaining_ms(Clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  if (remaining.count() <= 0)
    return 0;
  return static_cast<int>(std::min<std::int64_t>(remaining.count(), 60000));
}

bool bounded_text(const std::string &value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum;
}
} // namespace

struct ProcessWorkerTransport::Impl {
  explicit Impl(std::string worker_id, ProcessWorkerConfig worker_config)
      : id(std::move(worker_id)), config(std::move(worker_config)) {
    metadata_.id = id;
    metadata_.name = id;
    metadata_.plugin = "process";
    metadata_.event_source_id = "worker." + id;
    metadata_.local = true;
    metadata_.remote = false;
    metadata_.enabled = true;
    metadata_.status = "stopped";
  }

  ~Impl() { stop(); }

  WorkerMetadata metadata() const {
    std::lock_guard lock(mutex);
    return metadata_;
  }

  void start() {
    std::lock_guard lock(mutex);
    if (pid > 0)
      return;
    metadata_.status = "starting";
    metadata_.healthy = false;
    try {
      spawn_locked();
      const auto response = request_locked(
          "hello", "", "", Json{{"client", "laso"}}, config.startup_timeout_ms);
      if (!response.value("ok", false))
        throw WorkerTransportError("Worker hello was rejected");
      if (!response.contains("metadata") || !response.at("metadata").is_object() ||
          response.at("metadata").dump().size() > process_protocol::max_metadata_bytes)
        throw WorkerTransportError("Worker hello metadata is invalid");
      auto reported = response.at("metadata").get<WorkerMetadata>();
      metadata_ = std::move(reported);
      metadata_.id = id;
      metadata_.plugin = "process";
      metadata_.event_source_id = "worker." + id;
      metadata_.local = true;
      metadata_.remote = false;
      metadata_.enabled = true;
      metadata_.healthy = true;
      metadata_.status = "healthy";
    } catch (...) {
      terminate_locked();
      metadata_.healthy = false;
      metadata_.status = "failed";
      throw;
    }
  }

  WorkerSubmission submit(const WorkerRequest &request) {
    std::lock_guard lock(mutex);
    ensure_started_locked();
    Json payload{{"job_id", request.job_id},
                 {"worker_id", request.worker_id},
                 {"capability", request.capability},
                 {"task_type", request.task_type},
                 {"instructions", request.instructions},
                 {"deadline", request.deadline},
                 {"idempotency_key", request.idempotency_key},
                 {"run_id", request.run_id},
                 {"node_id", request.node_id},
                 {"attempt", request.attempt},
                 {"input", request.input},
                 {"output_schema", request.output_schema},
                 {"metadata", request.metadata},
                 {"artifact_ids", request.artifact_ids}};
    return parse_submission_locked(request.job_id, request_response_locked("submit", request.job_id,
                                                                            "", payload));
  }

  WorkerStatus status(const std::string &external_job_id) {
    return status_operation("status", external_job_id);
  }

  WorkerStatus result(const std::string &external_job_id) {
    return status_operation("result", external_job_id);
  }

  bool cancel(const std::string &external_job_id) {
    std::lock_guard lock(mutex);
    ensure_started_locked();
    const auto response = request_response_locked("cancel", "", external_job_id, Json::object());
    if (!response.value("ok", false))
      throw WorkerTransportError("Worker cancellation was rejected");
    if (response.contains("acknowledged"))
      return response.at("acknowledged").is_boolean() && response.at("acknowledged").get<bool>();
    return response.value("payload", Json::object()).value("acknowledged", false);
  }

  void stop() noexcept {
    std::lock_guard lock(mutex);
    if (pid <= 0) {
      close_fd(input_fd);
      close_fd(output_fd);
      close_fd(error_fd);
      return;
    }
    try {
      (void)request_locked("shutdown", "", "", Json::object(), 500);
    } catch (...) {
    }
    terminate_locked();
    metadata_.healthy = false;
    metadata_.status = "stopped";
  }

private:
  WorkerMetadata metadata_;
  std::string id;
  ProcessWorkerConfig config;
  mutable std::mutex mutex;
  pid_t pid = -1;
  int input_fd = -1, output_fd = -1, error_fd = -1;
  std::uint64_t request_number = 0;
  std::string stderr_capture;

  void ensure_started_locked() {
    if (pid <= 0 || !metadata_.healthy)
      throw WorkerTransportError("Worker process is unavailable");
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      mark_dead_locked();
      throw WorkerTransportError("Worker process exited unexpectedly");
    }
  }

  void spawn_locked() {
    int child_input[2], child_output[2], child_error[2];
    if (::pipe(child_input) < 0 || ::pipe(child_output) < 0 || ::pipe(child_error) < 0) {
      close_fd(child_input[0]);
      close_fd(child_input[1]);
      close_fd(child_output[0]);
      close_fd(child_output[1]);
      close_fd(child_error[0]);
      close_fd(child_error[1]);
      throw WorkerTransportError("Unable to create worker process pipes");
    }
    std::vector<std::string> inherited;
    for (const auto &name : config.environment_allowlist)
      if (const auto *value = std::getenv(name.c_str()))
        inherited.push_back(name + "=" + value);
    for (const auto &[name, value] : config.environment)
      inherited.push_back(name + "=" + value);
    pid = ::fork();
    if (pid < 0) {
      close_fd(child_input[0]);
      close_fd(child_input[1]);
      close_fd(child_output[0]);
      close_fd(child_output[1]);
      close_fd(child_error[0]);
      close_fd(child_error[1]);
      throw WorkerTransportError("Unable to start worker process");
    }
    if (pid == 0) {
      ::setpgid(0, 0);
      ::dup2(child_input[0], STDIN_FILENO);
      ::dup2(child_output[1], STDOUT_FILENO);
      ::dup2(child_error[1], STDERR_FILENO);
      close_fd(child_input[0]);
      close_fd(child_input[1]);
      close_fd(child_output[0]);
      close_fd(child_output[1]);
      close_fd(child_error[0]);
      close_fd(child_error[1]);
      std::vector<char *> environment;
      for (auto &entry : inherited)
        environment.push_back(entry.data());
      environment.push_back(nullptr);
      std::vector<char *> arguments;
      arguments.push_back(const_cast<char *>(config.executable.c_str()));
      for (auto &arg : config.args)
        arguments.push_back(arg.data());
      arguments.push_back(nullptr);
      ::execve(config.executable.c_str(), arguments.data(), environment.data());
      _exit(127);
    }
    close_fd(child_input[0]);
    close_fd(child_output[1]);
    close_fd(child_error[1]);
    input_fd = child_input[1];
    output_fd = child_output[0];
    error_fd = child_error[0];
    try {
      nonblocking(input_fd);
      nonblocking(output_fd);
      nonblocking(error_fd);
    } catch (...) {
      terminate_locked();
      throw;
    }
    (void)::setpgid(pid, pid);
  }

  void mark_dead_locked() {
    pid = -1;
    close_fd(input_fd);
    close_fd(output_fd);
    close_fd(error_fd);
    metadata_.healthy = false;
    metadata_.status = "failed";
  }

  void terminate_locked() noexcept {
    if (pid > 0) {
      (void)::kill(-pid, SIGTERM);
      const auto deadline = Clock::now() + std::chrono::milliseconds(500);
      int status = 0;
      while (Clock::now() < deadline) {
        const auto result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid)
          break;
        if (result < 0 && errno == ECHILD)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (::waitpid(pid, &status, WNOHANG) == 0) {
        (void)::kill(-pid, SIGKILL);
        (void)::waitpid(pid, &status, 0);
      }
    }
    pid = -1;
    close_fd(input_fd);
    close_fd(output_fd);
    close_fd(error_fd);
  }

  void write_frame_locked(const std::string &frame, Clock::time_point deadline) {
    if (frame.size() > process_protocol::max_frame_bytes)
      throw WorkerTransportError("Worker request exceeds the frame limit");
    std::string wire = frame + '\n';
    std::size_t offset = 0;
    while (offset < wire.size()) {
      pollfd descriptor{input_fd, POLLOUT, 0};
      if (::poll(&descriptor, 1, remaining_ms(deadline)) <= 0)
        throw WorkerTransportError("Worker request timed out");
      if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        throw WorkerTransportError("Worker process closed its input");
      const auto written = ::write(input_fd, wire.data() + offset, wire.size() - offset);
      if (written > 0)
        offset += static_cast<std::size_t>(written);
      else if (written < 0 && errno != EAGAIN && errno != EINTR)
        throw WorkerTransportError("Unable to write worker request");
    }
  }

  void drain_stderr_locked(short revents) {
    if (!(revents & (POLLIN | POLLHUP | POLLERR)))
      return;
    char buffer[4096];
    for (;;) {
      const auto count = ::read(error_fd, buffer, sizeof(buffer));
      if (count > 0) {
        if (stderr_capture.size() > process_protocol::max_stderr_bytes -
                                  static_cast<std::size_t>(count))
          throw WorkerTransportError("Worker stderr exceeds the capture limit");
        stderr_capture.append(buffer, static_cast<std::size_t>(count));
      } else if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
        return;
      } else {
        close_fd(error_fd);
        return;
      }
    }
  }

  std::string read_frame_locked(Clock::time_point deadline) {
    std::string frame;
    char buffer[4096];
    for (;;) {
      pollfd descriptors[2] = {{output_fd, POLLIN, 0}, {error_fd, POLLIN, 0}};
      const auto count = ::poll(descriptors, error_fd >= 0 ? 2 : 1, remaining_ms(deadline));
      if (count == 0)
        throw WorkerTransportError("Worker response timed out");
      if (count < 0) {
        if (errno == EINTR)
          continue;
        throw WorkerTransportError("Unable to read worker response");
      }
      if (error_fd >= 0)
        drain_stderr_locked(descriptors[1].revents);
      if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        const auto received = ::read(output_fd, buffer, sizeof(buffer));
        if (received > 0) {
          frame.append(buffer, static_cast<std::size_t>(received));
          const auto newline = frame.find('\n');
          if (newline != std::string::npos) {
            if (newline > process_protocol::max_frame_bytes ||
                frame.size() > newline + 1)
              throw WorkerTransportError("Worker sent multiple or oversized frames");
            frame.resize(newline);
            if (!frame.empty() && frame.back() == '\r')
              frame.pop_back();
            return frame;
          }
          if (frame.size() > process_protocol::max_frame_bytes)
            throw WorkerTransportError("Worker response exceeds the frame limit");
        } else if (received == 0) {
          throw WorkerTransportError("Worker response was truncated");
        } else if (errno != EAGAIN && errno != EINTR) {
          throw WorkerTransportError("Unable to read worker response");
        }
      }
      if (pid > 0) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
          throw WorkerTransportError("Worker process exited unexpectedly");
      }
    }
  }

  Json request_locked(const std::string &operation, const std::string &job_id,
                     const std::string &external_job_id, const Json &payload,
                     std::uint64_t timeout_ms) {
    const auto request_id = "req-" + std::to_string(++request_number);
    Json request{{"protocol_version", process_protocol::version},
                 {"request_id", request_id},
                 {"operation", operation},
                 {"job_id", job_id},
                 {"external_job_id", external_job_id},
                 {"payload", payload}};
    const auto wire = request.dump();
    if (wire.size() > process_protocol::max_frame_bytes)
      throw WorkerTransportError("Worker request exceeds the frame limit");
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    write_frame_locked(wire, deadline);
    const auto response_wire = read_frame_locked(deadline);
    const auto response = Json::parse(response_wire, nullptr, false);
    if (response.is_discarded() || !response.is_object())
      throw WorkerTransportError("Worker response is not valid JSON");
    if (response.value("protocol_version", 0U) != process_protocol::version)
      throw WorkerTransportError("Worker protocol version is incompatible");
    if (response.value("request_id", std::string{}) != request_id)
      throw WorkerTransportError("Worker response request id does not match");
    if (!response.contains("ok") || !response.at("ok").is_boolean())
      throw WorkerTransportError("Worker response has no valid success flag");
    if (!response.at("ok").get<bool>())
      throw WorkerTransportError("Worker rejected protocol request");
    return response;
  }

  Json request_response_locked(const std::string &operation, const std::string &job_id,
                               const std::string &external_job_id, const Json &payload) {
    return request_locked(operation, job_id, external_job_id, payload, config.request_timeout_ms);
  }

  static WorkerJobState response_state(const Json &response) {
    if (!response.contains("state"))
      throw WorkerTransportError("Worker response has no state");
    try {
      return response.at("state").get<WorkerJobState>();
    } catch (...) {
      throw WorkerTransportError("Worker response has an invalid state");
    }
  }

  static WorkerUsage response_usage(const Json &response) {
    if (!response.contains("usage") || response.at("usage").is_null())
      return {};
    try {
      return response.at("usage").get<WorkerUsage>();
    } catch (...) {
      throw WorkerTransportError("Worker response has invalid usage");
    }
  }

  static Json response_payload(const Json &response) {
    const auto payload = response.value("payload", Json::object());
    if (payload.dump().size() > process_protocol::max_frame_bytes)
      throw WorkerTransportError("Worker payload exceeds the frame limit");
    return payload;
  }

  static void validate_response_metadata(const Json &response) {
    const auto metadata = response.value("metadata", Json::object());
    if (!metadata.is_object() || metadata.dump().size() > process_protocol::max_metadata_bytes)
      throw WorkerTransportError("Worker response metadata exceeds the limit");
    const auto artifacts = response.value("artifacts", std::vector<Json>{});
    if (artifacts.size() > process_protocol::max_artifact_references)
      throw WorkerTransportError("Worker response has too many artifact references");
    for (const auto &artifact : artifacts)
      if (!artifact.is_object() || artifact.dump().size() > process_protocol::max_metadata_bytes)
        throw WorkerTransportError("Worker artifact reference is invalid");
    const auto error = response.value("error", std::string{});
    if (error.size() > max_error_bytes)
      throw WorkerTransportError("Worker error exceeds the limit");
  }

  static WorkerSubmission parse_submission_locked(const std::string &, const Json &response) {
    validate_response_metadata(response);
    WorkerSubmission result;
    result.external_job_id = response.value("external_job_id", std::string{});
    if (!bounded_text(result.external_job_id, max_id_bytes))
      throw WorkerTransportError("Worker returned an invalid external job id");
    result.state = response_state(response);
    result.metadata = response.value("metadata", Json::object());
    result.result = response_payload(response);
    result.artifacts = response.value("artifacts", std::vector<Json>{});
    result.error = response.value("error", std::string{});
    result.usage = response_usage(response);
    return result;
  }

  WorkerStatus status_operation(const std::string &operation, const std::string &external_job_id) {
    std::lock_guard lock(mutex);
    ensure_started_locked();
    const auto response = request_response_locked(operation, "", external_job_id, Json::object());
    validate_response_metadata(response);
    WorkerStatus result;
    result.state = response_state(response);
    result.result = response_payload(response);
    result.metadata = response.value("metadata", Json::object());
    result.artifacts = response.value("artifacts", std::vector<Json>{});
    result.error = response.value("error", std::string{});
    result.usage = response_usage(response);
    return result;
  }
};

ProcessWorkerTransport::ProcessWorkerTransport(std::string id, ProcessWorkerConfig config)
    : impl_(std::make_unique<Impl>(std::move(id), std::move(config))) {}
ProcessWorkerTransport::~ProcessWorkerTransport() noexcept = default;
WorkerMetadata ProcessWorkerTransport::metadata() const { return impl_->metadata(); }
WorkerSubmission ProcessWorkerTransport::submit(const WorkerRequest &request) {
  return impl_->submit(request);
}
WorkerStatus ProcessWorkerTransport::status(const std::string &external_job_id) {
  return impl_->status(external_job_id);
}
WorkerStatus ProcessWorkerTransport::result(const std::string &external_job_id) {
  return impl_->result(external_job_id);
}
bool ProcessWorkerTransport::cancel(const std::string &external_job_id) {
  return impl_->cancel(external_job_id);
}
void ProcessWorkerTransport::start() { impl_->start(); }
void ProcessWorkerTransport::stop() noexcept { impl_->stop(); }
} // namespace laso
