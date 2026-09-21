#include "../support.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <laso/api/api.hpp>
#include <laso/workers/manager.hpp>
#include <thread>

using namespace laso;
using namespace laso::test;

namespace {
std::string worker_pipeline(const std::string &input_schema = "",
                            const std::string &output_schema = "") {
  return std::string{"laso: '1'\nname: worker-contract\nversion: 1\n"} +
         "nodes:\n  work:\n    type: worker\n    worker: offline\n    task_type: deterministic\n" +
         (input_schema.empty() ? "" : "    input_schema: " + input_schema + "\n") +
         (output_schema.empty() ? "" : "    output_schema: " + output_schema + "\n") +
         "edges:\n  - {from: input, to: work}\n  - {from: work, to: output}\n";
}

Config worker_config(const std::filesystem::path &dir) {
  auto result = config(dir);
  result.plugin_dirs = {LASO_WORKER_PLUGIN_DIR};
  result.worker_plugins.emplace(
      "offline", WorkerConfig{"example-worker", "example-worker", "", true, Json::object()});
  return result;
}
} // namespace

TEST(Workers, PluginLoadsStartsAndReportsHealth) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service service(io, worker_config(dir.path));
  const auto workers = service.workers();
  ASSERT_EQ(workers.size(), 1U);
  EXPECT_EQ(workers.front().at("id"), "offline");
  EXPECT_EQ(workers.front().at("plugin"), "example-worker");
  EXPECT_EQ(workers.front().at("status"), "healthy");
  EXPECT_TRUE(workers.front().at("supports_recovery"));
  LocalDevelopmentIdentity identity;
  Api api(service, identity);
  EXPECT_EQ(api.handle("GET", "/api/v1/workers", "").status, 200U);
  EXPECT_EQ(api.handle("GET", "/api/v1/workers/offline", "").status, 200U);
}

TEST(Workers, WorkerNodeUsesNormalRuntimeAndPersistsJob) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service service(io, worker_config(dir.path));
  const auto run = execute(service, io, worker_pipeline(), {{"value", 7}});
  EXPECT_EQ(run.state, RunState::Completed);
  EXPECT_FALSE(run.worker_job_id.empty());
  EXPECT_TRUE(run.message.payload.at("ok"));
  EXPECT_EQ(run.message.payload.at("worker"), "offline-example");
  const auto jobs = service.worker_jobs(run.id);
  ASSERT_EQ(jobs.size(), 1U);
  EXPECT_EQ(jobs.front().at("status"), "Completed");
  EXPECT_EQ(jobs.front().at("run_id"), run.id);
  EXPECT_EQ(jobs.front().at("worker_id"), "offline");
  const auto attempts = service.list(RecordKind::Attempt, run.id);
  const auto worker_attempt =
      std::find_if(attempts.begin(), attempts.end(), [](const Json &attempt) {
        return !attempt.value("worker_job_id", std::string{}).empty();
      });
  ASSERT_NE(worker_attempt, attempts.end());
  EXPECT_EQ(worker_attempt->at("worker_job_id"), jobs.front().at("id"));
  EXPECT_EQ(worker_attempt->at("external_job_id"), jobs.front().at("external_job_id"));
  const auto events = service.list(RecordKind::Event, run.id);
  EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const Json &event) {
    return event.at("type") == "worker.completed";
  }));
  EXPECT_TRUE(
      std::any_of(run.message.provenance.begin(), run.message.provenance.end(),
                  [](const ProvenanceRecord &record) { return record.worker == "offline"; }));
}

TEST(Workers, InputAndOutputSchemasApplyAtWorkerBoundary) {
  TemporaryDirectory dir;
  std::ofstream(dir.path / "input.json")
      << R"({"type":"object","required":["value"],"properties":{"value":{"type":"integer"}}})";
  std::ofstream(dir.path / "output.json")
      << R"({"type":"object","required":["ok","worker"],"properties":{"ok":{"const":true},"worker":{"type":"string"}}})";
  asio::io_context io;
  auto c = worker_config(dir.path);
  c.schema_roots = {dir.path};
  Service service(io, c);
  const auto valid =
      execute(service, io, worker_pipeline("input.json", "output.json"), {{"value", 9}});
  EXPECT_EQ(valid.state, RunState::Completed);
  const auto invalid =
      execute(service, io, worker_pipeline("input.json", "output.json"), {{"wrong", 9}});
  EXPECT_EQ(invalid.state, RunState::Failed);
  EXPECT_TRUE(service.worker_jobs(invalid.id).empty());
}

TEST(Workers, PolicyApprovalGuardsWorkerExecution) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto c = worker_config(dir.path);
  c.rules = {{"offline", PolicyDecision::RequireApproval}};
  Service service(io, c);
  const auto pipeline = service.register_pipeline(worker_pipeline());
  const auto run_id = service.start(pipeline.at("id"), {{"value", 3}});
  io.run();
  auto waiting = service.get(RecordKind::Run, run_id).get<laso::Run>();
  ASSERT_EQ(waiting.state, RunState::WaitingApproval);
  const auto approval = service.list(RecordKind::Approval, run_id).front().get<Approval>();
  service.runtime().decide(approval.id, true, "tester", "approved");
  io.restart();
  io.run();
  EXPECT_EQ(service.get(RecordKind::Run, run_id).get<laso::Run>().state, RunState::Completed);
}

TEST(Workers, WorkerJobIdempotencyAvoidsResubmission) {
  class ImmediateWorker final : public WorkerAdapter {
  public:
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = "fake";
      result.name = "fake";
      result.enabled = true;
      result.healthy = true;
      result.status = "healthy";
      result.supports_recovery = true;
      result.supports_cancellation = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      ++submissions;
      return {"external-1", WorkerJobState::Queued, Json::object()};
    }
    WorkerStatus status(const std::string &) override {
      return {WorkerJobState::Queued, nullptr, Json::object(), {}, {}};
    }
    WorkerStatus result(const std::string &) override {
      return {WorkerJobState::Completed, Json{{"ok", true}}, Json::object(), {}, {}};
    }
    bool cancel(const std::string &) override {
      return true;
    }
    void start() override {}
    void stop() noexcept override {}
    unsigned submissions = 0;
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<ImmediateWorker>();
  registry.add("fake", adapter);
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "fake";
  request.run_id = "run-1";
  request.node_id = "node-1";
  request.idempotency_key = "run-1:node-1:1";
  const auto first = manager.submit(request);
  const auto second = manager.submit(request);
  EXPECT_EQ(first.id, second.id);
  EXPECT_EQ(adapter->submissions, 1U);
  EXPECT_EQ(storage->list(RecordKind::WorkerJob, "run-1").size(), 1U);
}

TEST(Workers, ConcurrentIdempotentSubmissionCreatesOneDurableJob) {
  class CountingWorker final : public WorkerAdapter {
  public:
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = "counting";
      result.name = "counting";
      result.enabled = true;
      result.healthy = true;
      result.status = "healthy";
      result.supports_recovery = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      ++submissions;
      return {"external-counting", WorkerJobState::Queued, Json::object()};
    }
    WorkerStatus status(const std::string &) override {
      return {WorkerJobState::Queued, nullptr, Json::object(), {}, {}};
    }
    WorkerStatus result(const std::string &) override {
      return {WorkerJobState::Completed, Json{{"ok", true}}, Json::object(), {}, {}};
    }
    bool cancel(const std::string &) override {
      return true;
    }
    void start() override {}
    void stop() noexcept override {}
    std::atomic<unsigned> submissions = 0;
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<CountingWorker>();
  registry.add("counting", adapter);
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "counting";
  request.run_id = "run-concurrent";
  request.node_id = "work";
  request.attempt = 1;
  request.idempotency_key = "run-concurrent:work:1";
  std::string first, second;
  std::jthread left([&] { first = manager.submit(request).id; });
  std::jthread right([&] { second = manager.submit(request).id; });
  left.join();
  right.join();
  EXPECT_EQ(first, second);
  EXPECT_EQ(adapter->submissions.load(), 1U);
  EXPECT_EQ(storage->list(RecordKind::WorkerJob, request.run_id).size(), 1U);
}

TEST(Workers, TerminalWorkerJobIgnoresLateCompletionAndFailedCancellation) {
  class QueuedWorker final : public WorkerAdapter {
  public:
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = "queued";
      result.name = "queued";
      result.event_source_id = "worker.queued";
      result.enabled = true;
      result.healthy = true;
      result.status = "healthy";
      result.supports_recovery = true;
      result.supports_cancellation = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      return {"external-queued", WorkerJobState::Queued, Json::object()};
    }
    WorkerStatus status(const std::string &) override {
      return {WorkerJobState::Queued, nullptr, Json::object(), {}, {}};
    }
    WorkerStatus result(const std::string &) override {
      return {};
    }
    bool cancel(const std::string &) override {
      return false;
    }
    void start() override {}
    void stop() noexcept override {}
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  registry.add("queued", std::make_shared<QueuedWorker>());
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "queued";
  request.run_id = "run-race";
  request.node_id = "work";
  request.idempotency_key = "run-race:work:1";
  const auto created = manager.submit(request);
  Event completed;
  completed.source_id = "worker.queued";
  completed.type = "worker.job.completed";
  completed.payload = {
      {"job_id", created.id}, {"external_job_id", "external-queued"}, {"result", {{"ok", true}}}};
  manager.receive(completed);
  EXPECT_EQ(manager.job(created.id).state, WorkerJobState::Completed);
  manager.cancel(created.id, WorkerJobState::Cancelled, "test cancellation");
  EXPECT_EQ(manager.job(created.id).state, WorkerJobState::Completed);
}

TEST(Workers, ExistingWorkerJobIsReconciledAfterManagerRestart) {
  class RecoveringWorker final : public WorkerAdapter {
  public:
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = "recovering";
      result.name = "recovering";
      result.enabled = true;
      result.healthy = true;
      result.status = "healthy";
      result.supports_recovery = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      ++submissions;
      return {"external-recovering", WorkerJobState::Queued, Json::object()};
    }
    WorkerStatus status(const std::string &) override {
      return {WorkerJobState::Completed, Json{{"recovered", true}}, Json::object(), {}, {}};
    }
    WorkerStatus result(const std::string &) override {
      return {};
    }
    bool cancel(const std::string &) override {
      return true;
    }
    void start() override {}
    void stop() noexcept override {}
    std::atomic<unsigned> submissions = 0;
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<RecoveringWorker>();
  registry.add("recovering", adapter);
  WorkerRequest request;
  request.worker_id = "recovering";
  request.run_id = "run-recovery";
  request.node_id = "work";
  request.idempotency_key = "run-recovery:work:1";
  {
    WorkerManager manager(*storage, registry);
    EXPECT_EQ(manager.submit(request).state, WorkerJobState::Queued);
  }
  {
    WorkerManager restarted(*storage, registry);
    const auto recovered = restarted.submit(request);
    EXPECT_EQ(recovered.state, WorkerJobState::Completed);
    EXPECT_EQ(recovered.result.at("recovered"), true);
    EXPECT_EQ(adapter->submissions.load(), 1U);
  }
}
