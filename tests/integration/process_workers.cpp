#include "../support.hpp"
#include <atomic>
#include <boost/beast.hpp>
#include <cstdlib>
#include <fstream>
#include <laso/api/api.hpp>
#include <laso/workers/process_transport.hpp>
#include <thread>

using namespace laso;
using namespace laso::test;

namespace {
ProcessWorkerConfig worker_config(const std::string &mode, std::uint64_t timeout = 500) {
  ProcessWorkerConfig result;
  result.executable = LASO_PROCESS_WORKER_HOST;
  result.args = {"--mode", mode};
  result.startup_timeout_ms = timeout;
  result.request_timeout_ms = timeout;
  return result;
}

WorkerRequest request(const std::string &worker = "process") {
  WorkerRequest result;
  result.job_id = "job-process-1";
  result.worker_id = worker;
  result.task_type = "deterministic";
  result.instructions = "return the structured input";
  result.idempotency_key = "process-idempotency-1";
  result.run_id = "run-process-1";
  result.node_id = "work";
  result.input = {{"value", 7}};
  return result;
}

bool reference_host_running() {
  const std::filesystem::path proc("/proc");
  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator(proc, error)) {
    if (error || !entry.is_directory())
      continue;
    const auto name = entry.path().filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit))
      continue;
    std::ifstream command(entry.path() / "cmdline", std::ios::binary);
    std::string first;
    std::getline(command, first, '\0');
    if (first == LASO_PROCESS_WORKER_HOST)
      return true;
  }
  return false;
}

boost::beast::http::response<boost::beast::http::string_body>
http_request(unsigned short port, boost::beast::http::verb method,
             const std::string &target, const Json &body = Json::object()) {
  namespace http = boost::beast::http;
  asio::io_context peer_io;
  boost::beast::tcp_stream stream(peer_io);
  stream.expires_after(std::chrono::seconds(5));
  stream.connect({asio::ip::make_address("127.0.0.1"), port});
  http::request<http::string_body> request{method, target, 11};
  request.set(http::field::host, "localhost");
  if (method != http::verb::get)
    request.body() = body.dump();
  request.prepare_payload();
  http::write(stream, request);
  boost::beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(stream, buffer, response);
  return response;
}

std::string process_pipeline() {
  return "laso: '1'\nname: process-worker-e2e\nversion: 1\n"
         "nodes:\n  work:\n    type: worker\n    worker: process\n"
         "    task_type: deterministic\n    instructions: test process boundary\n"
         "edges:\n  - {from: input, to: work}\n  - {from: work, to: output}\n";
}
} // namespace

TEST(ProcessWorker, HandshakeAndSubmitStatusResultLifecycle) {
  ProcessWorkerTransport transport("process", worker_config("success"));
  EXPECT_NO_THROW(transport.start());
  ASSERT_TRUE(transport.metadata().healthy);
  EXPECT_TRUE(transport.metadata().local);
  const auto submission = transport.submit(request());
  EXPECT_EQ(submission.state, WorkerJobState::Completed);
  EXPECT_EQ(submission.external_job_id, "process-job-process-1");
  EXPECT_TRUE(submission.result.at("ok"));
  EXPECT_EQ(submission.usage.total_tokens, std::optional<std::uint64_t>(5));
  EXPECT_EQ(transport.status(submission.external_job_id).state, WorkerJobState::Completed);
  EXPECT_EQ(transport.result(submission.external_job_id).result.at("worker"), "process-reference");
}

TEST(ProcessWorker, VersionMismatchIsTransportFailure) {
  ProcessWorkerTransport transport("process", worker_config("mismatch"));
  EXPECT_THROW(transport.start(), WorkerTransportError);
  EXPECT_EQ(transport.metadata().status, "failed");
}

TEST(ProcessWorker, UsageIsOptionalAndPropagatedWhenReported) {
  ProcessWorkerTransport without_usage("process", worker_config("no-usage"));
  without_usage.start();
  EXPECT_FALSE(without_usage.submit(request()).usage.total_tokens.has_value());
  ProcessWorkerTransport with_usage("process", worker_config("usage"));
  with_usage.start();
  const auto result = with_usage.submit(request());
  EXPECT_EQ(result.usage.executor, "reference-worker");
  EXPECT_EQ(result.usage.input_tokens, std::optional<std::uint64_t>(3));
  EXPECT_EQ(result.usage.output_tokens, std::optional<std::uint64_t>(2));
}

TEST(ProcessWorker, ExplicitEnvironmentOverridesParentWithoutImplicitInheritance) {
  struct EnvironmentGuard {
    ~EnvironmentGuard() {
      (void)::unsetenv("LASO_PARENT_SECRET");
    }
  } guard;
  ASSERT_EQ(::setenv("LASO_PARENT_SECRET", "must-not-cross-process-boundary", 1), 0);
  auto config = worker_config("environment");
  config.environment["LASO_REFERENCE"] = "enabled";
  ProcessWorkerTransport transport("process", std::move(config));
  ASSERT_NO_THROW(transport.start());
  const auto result = transport.submit(request());
  EXPECT_FALSE(result.result.at("parent_secret_inherited"));
  EXPECT_EQ(result.result.at("explicit_override"), "enabled");
  transport.stop();
}

TEST(ProcessWorker, WorkerFailureIsDistinctFromTransportFailure) {
  ProcessWorkerTransport transport("process", worker_config("failure"));
  transport.start();
  const auto result = transport.submit(request());
  EXPECT_EQ(result.state, WorkerJobState::Failed);
  EXPECT_EQ(result.error, "reference worker declared failure");
  ProcessWorkerTransport broken("process", worker_config("crash"));
  broken.start();
  EXPECT_THROW(broken.submit(request()), WorkerTransportError);
}

TEST(ProcessWorker, MalformedOversizedExitAndHangAreBoundedFailures) {
  for (const auto &mode :
       {std::string("malformed"), std::string("oversized"), std::string("truncated"),
        std::string("exit-after-hello"), std::string("hang")}) {
    ProcessWorkerTransport transport("process", worker_config(mode, 150));
    ASSERT_NO_THROW(transport.start()) << mode;
    EXPECT_THROW(transport.submit(request()), WorkerTransportError) << mode;
    EXPECT_FALSE(transport.metadata().healthy);
  }
}

TEST(ProcessWorker, CooperativeCancellationIsAcknowledged) {
  ProcessWorkerTransport transport("process", worker_config("cancel"));
  transport.start();
  const auto submission = transport.submit(request());
  ASSERT_EQ(submission.state, WorkerJobState::Queued);
  EXPECT_TRUE(transport.cancel(submission.external_job_id));
  EXPECT_EQ(transport.status(submission.external_job_id).state, WorkerJobState::Cancelled);
}

TEST(ProcessWorker, WorkerInteractionIsCorrelatedAndAnsweredByLASO) {
  ProcessWorkerTransport transport("process", worker_config("interaction", 1500));
  std::atomic<unsigned> requests = 0;
  transport.set_interaction_handler([&](const WorkerInteractionRequest &interaction) {
    ++requests;
    EXPECT_EQ(interaction.type, WorkerInteractionType::Permission);
    EXPECT_EQ(interaction.worker_job_id, "job-process-1");
    return WorkerInteractionResponse{interaction.request_id, WorkerInteractionState::Approved,
                                     Json{{"scope", "once"}}, "approved by test policy"};
  });
  transport.start();
  const auto submission = transport.submit(request());
  EXPECT_EQ(submission.state, WorkerJobState::Completed);
  EXPECT_EQ(requests.load(), 1U);
}

TEST(ProcessWorker, ConfigurationUsesExplicitExecutableAndEnvironmentBoundary) {
  TemporaryDirectory dir;
  const auto path = dir.path / "process.yaml";
  std::ofstream(path) << "process_workers:\n  example:\n    executable: "
                      << LASO_PROCESS_WORKER_HOST
                      << "\n    args: [--mode, success]\n    environment_allowlist: [PATH]\n"
                         "    environment: {LASO_REFERENCE: enabled}\n    startup_timeout_ms: 700\n"
                         "    request_timeout_ms: 800\n";
  const auto loaded = load_config(path);
  ASSERT_EQ(loaded.process_workers.size(), 1U);
  EXPECT_EQ(loaded.process_workers.at("example").executable, LASO_PROCESS_WORKER_HOST);
  EXPECT_EQ(loaded.process_workers.at("example").args.size(), 2U);
  EXPECT_EQ(loaded.process_workers.at("example").environment.at("LASO_REFERENCE"), "enabled");
  EXPECT_EQ(loaded.process_workers.at("example").startup_timeout_ms, 700U);
}

TEST(ProcessWorker, ShutdownDoesNotLeaveReferenceChildRunning) {
  EXPECT_FALSE(reference_host_running());
  {
    ProcessWorkerTransport transport("process", worker_config("success"));
    transport.start();
    EXPECT_TRUE(reference_host_running());
    transport.stop();
    EXPECT_FALSE(reference_host_running());
  }
  EXPECT_FALSE(reference_host_running());
}

TEST(ProcessWorker, ServiceExecutesPipelineThroughSeparateProcess) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto config = laso::test::config(dir.path);
  config.process_workers.emplace("process", worker_config("success"));
  config.validate();
  Service service(io, config);
  ASSERT_EQ(service.worker("process").at("status"), "healthy");
  const auto run = execute(service, io, process_pipeline(), {{"value", 7}});
  EXPECT_EQ(run.state, RunState::Completed);
  EXPECT_TRUE(run.message.payload.at("ok"));
  EXPECT_EQ(run.message.payload.at("worker"), "process-reference");
  const auto jobs = service.worker_jobs(run.id);
  ASSERT_EQ(jobs.size(), 1U);
  EXPECT_EQ(jobs.front().at("status"), "Completed");
  EXPECT_EQ(jobs.front().at("failure_kind"), "none");
  EXPECT_EQ(jobs.front().at("usage").at("total_tokens"), 5);
}

TEST(ProcessWorker, ServicePollsQueuedProcessWorkerToCompletion) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto config = laso::test::config(dir.path);
  config.process_workers.emplace("process", worker_config("delay"));
  config.validate();
  Service service(io, config);
  const auto run = execute(service, io, process_pipeline(), {{"value", 7}});
  EXPECT_EQ(run.state, RunState::Completed);
  EXPECT_EQ(run.message.payload.at("mode"), "delay");
}

TEST(ProcessWorker, ServiceRecordsTransportFailureWithoutResubmission) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto config = laso::test::config(dir.path);
  config.process_workers.emplace("process", worker_config("crash"));
  config.validate();
  Service service(io, config);
  const auto run = execute(service, io, process_pipeline(), {{"value", 7}});
  EXPECT_EQ(run.state, RunState::Failed);
  const auto jobs = service.worker_jobs(run.id);
  ASSERT_EQ(jobs.size(), 1U);
  EXPECT_EQ(jobs.front().at("failure_kind"), "transport");
}

TEST(ProcessWorker, HttpApiRemainsResponsiveDuringPendingInteraction) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto config = laso::test::config(dir.path);
  auto process = worker_config("interaction", 5000);
  process.interaction_timeout_ms = 5000;
  config.process_workers.emplace("process", std::move(process));
  config.rules.push_back({"worker.reference", PolicyDecision::RequireApproval});
  config.validate();
  Service service(io, config);
  service.register_pipeline(process_pipeline());
  LocalDevelopmentIdentity identity;
  Api api(service, identity);
  HttpServer server(io, api, "127.0.0.1", 0);
  server.start();
  std::jthread executor([&] { io.run(); });

  const auto started = http_request(
      server.port(), boost::beast::http::verb::post,
      "/api/v1/pipelines/process-worker-e2e/runs", {{"input", {{"value", 7}}}});
  ASSERT_EQ(started.result_int(), 202);
  const auto run_id = Json::parse(started.body()).at("id").get<std::string>();

  std::string interaction_id;
  for (unsigned attempt = 0; attempt < 100 && interaction_id.empty(); ++attempt) {
    const auto listed = http_request(server.port(), boost::beast::http::verb::get,
                                     "/api/v1/worker-requests?limit=50");
    ASSERT_EQ(listed.result_int(), 200);
    for (const auto &item : Json::parse(listed.body()))
      if (item.value("run_id", std::string{}) == run_id &&
          item.value("state", std::string{}) == "pending") {
        interaction_id = item.at("id").get<std::string>();
        break;
      }
    if (interaction_id.empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_FALSE(interaction_id.empty());

  const auto approved = http_request(
      server.port(), boost::beast::http::verb::post,
      "/api/v1/worker-requests/" + interaction_id + "/approve");
  ASSERT_EQ(approved.result_int(), 202);
  EXPECT_EQ(Json::parse(approved.body()).at("state"), "approved");

  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    const auto inspected = http_request(
        server.port(), boost::beast::http::verb::get, "/api/v1/runs/" + run_id);
    ASSERT_EQ(inspected.result_int(), 200);
    const auto run = Json::parse(inspected.body());
    if (run.value("state", std::string{}) == "Completed")
      break;
    ASSERT_NE(run.value("state", std::string{}), "Failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto completed = http_request(server.port(), boost::beast::http::verb::get,
                                      "/api/v1/runs/" + run_id);
  EXPECT_EQ(Json::parse(completed.body()).at("state"), "Completed");
  server.stop();
  service.shutdown();
  executor.join();
}
