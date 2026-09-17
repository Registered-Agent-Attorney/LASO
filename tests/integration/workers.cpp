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

class UsageWorker final : public WorkerTransport {
public:
  WorkerMetadata metadata() const override {
    WorkerMetadata result;
    result.id = "usage";
    result.name = "usage";
    result.enabled = true;
    result.healthy = true;
    result.status = "healthy";
    result.event_source_id = "worker.usage";
    result.supports_recovery = true;
    result.supports_cancellation = true;
    return result;
  }
  WorkerSubmission submit(const WorkerRequest &) override {
    if (transport_failure)
      throw WorkerTransportError("synthetic transport failure");
    WorkerSubmission result;
    result.external_job_id = "external-usage";
    result.state = job_failure ? WorkerJobState::Failed : WorkerJobState::Queued;
    result.usage = submission_usage;
    return result;
  }
  WorkerStatus status(const std::string &) override {
    WorkerStatus result;
    result.state = status_state;
    result.result = status_result;
    result.usage = status_usage;
    return result;
  }
  WorkerStatus result(const std::string &) override {
    return {};
  }
  bool cancel(const std::string &) override {
    return true;
  }
  void start() override {}
  void stop() noexcept override {}

  WorkerUsage submission_usage, status_usage;
  WorkerJobState status_state = WorkerJobState::Queued;
  Json status_result = nullptr;
  bool transport_failure = false, job_failure = false;
};
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

TEST(Workers, UsageMetadataIsOptionalPartialAndFullyNormalized) {
  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  registry.add("usage", adapter);
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "usage";
  request.run_id = "usage-run";
  request.node_id = "work";
  request.idempotency_key = "usage-none";
  const auto no_usage = manager.submit(request);
  EXPECT_TRUE(no_usage.usage.queue_duration_ms == std::nullopt);
  EXPECT_TRUE(no_usage.usage.total_tokens == std::nullopt);

  adapter->submission_usage.input_tokens = 12;
  request.idempotency_key = "usage-partial";
  const auto partial = manager.submit(request);
  ASSERT_TRUE(partial.usage.input_tokens.has_value());
  EXPECT_EQ(*partial.usage.input_tokens, 12U);
  EXPECT_TRUE(partial.usage.total_tokens == std::nullopt);

  adapter->submission_usage.output_tokens = 8;
  adapter->submission_usage.wall_duration_ms = 25;
  adapter->submission_usage.provider = "provider";
  adapter->submission_usage.model = "model";
  adapter->submission_usage.executor = "executor";
  adapter->submission_usage.tool_calls = 2;
  adapter->submission_usage.action_count = 3;
  adapter->submission_usage.cost_units = 0.5;
  adapter->submission_usage.metadata = {{"source", "test"}};
  request.idempotency_key = "usage-full";
  const auto full = manager.submit(request);
  ASSERT_TRUE(full.usage.total_tokens.has_value());
  EXPECT_EQ(*full.usage.total_tokens, 20U);
  EXPECT_EQ(*full.usage.input_tokens, 12U);
  EXPECT_EQ(*full.usage.output_tokens, 8U);
  EXPECT_EQ(full.usage.metadata.at("source"), "test");
  EXPECT_EQ(storage->get(RecordKind::WorkerJob, full.id).get<WorkerJob>().usage.model, "model");
}

TEST(Workers, UsageMetadataPersistsAcrossRestartAndCompletionEvents) {
  TemporaryDirectory dir;
  const auto path = dir.path / "state.db";
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  registry.add("usage", adapter);
  WorkerJob created;
  {
    auto storage = make_storage(path);
    WorkerManager manager(*storage, registry);
    WorkerRequest request;
    request.worker_id = "usage";
    request.run_id = "restart-usage";
    request.node_id = "work";
    request.idempotency_key = "restart-usage:1";
    created = manager.submit(request);
  }
  {
    auto storage = make_storage(path);
    WorkerManager manager(*storage, registry);
    Event completed;
    completed.source_id = "";
    completed.type = "worker.job.completed";
    completed.payload = {{"job_id", created.id},
                         {"external_job_id", "external-usage"},
                         {"result", {{"ok", true}}},
                         {"usage",
                          {{"queue_duration_ms", 4},
                           {"wall_duration_ms", 9},
                           {"input_tokens", 2},
                           {"output_tokens", 3},
                           {"cost_units", 0.25}}}};
    // The synthetic adapter has no event source identity; use the persisted
    // adapter identity only after replacing it with the expected source.
    completed.source_id = "worker.usage";
    manager.receive(completed);
    const auto recovered = manager.job(created.id);
    EXPECT_EQ(recovered.state, WorkerJobState::Completed);
    EXPECT_EQ(recovered.result.at("ok"), true);
    ASSERT_TRUE(recovered.usage.total_tokens.has_value());
    EXPECT_EQ(*recovered.usage.total_tokens, 5U);
    EXPECT_EQ(*recovered.usage.queue_duration_ms, 4U);
  }
}

TEST(Workers, WorkerBudgetsAcceptAndRejectDeterministically) {
  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  registry.add("usage", adapter);
  WorkerManager manager(*storage, registry, 32, 16, 16, 100, 10, 2.0);
  WorkerRequest request;
  request.worker_id = "usage";
  request.run_id = "budget-run";
  request.node_id = "work";
  adapter->submission_usage.wall_duration_ms = 50;
  adapter->submission_usage.total_tokens = 6;
  adapter->submission_usage.cost_units = 1.0;
  request.idempotency_key = "budget-accepted";
  EXPECT_EQ(manager.submit(request).state, WorkerJobState::Queued);

  adapter->submission_usage.wall_duration_ms = 101;
  adapter->submission_usage.total_tokens = 5;
  adapter->submission_usage.cost_units = 1.0;
  request.idempotency_key = "budget-wall";
  const auto wall_rejected = manager.submit(request);
  EXPECT_EQ(wall_rejected.state, WorkerJobState::Failed);
  EXPECT_EQ(wall_rejected.failure_kind, WorkerFailureKind::Budget);
  EXPECT_EQ(wall_rejected.error, "worker wall-time budget exceeded");

  adapter->submission_usage.wall_duration_ms = 50;
  request.idempotency_key = "budget-total";
  const auto total_rejected = manager.submit(request);
  EXPECT_EQ(total_rejected.state, WorkerJobState::Failed);
  EXPECT_EQ(total_rejected.failure_kind, WorkerFailureKind::Budget);
  EXPECT_EQ(total_rejected.error, "worker token budget exceeded");

  auto cost_storage = make_storage(dir.path / "cost.db");
  WorkerManager cost_manager(*cost_storage, registry, 32, 16, 16, 0, 0, 2.0);
  adapter->submission_usage.total_tokens = 1;
  adapter->submission_usage.cost_units = 1.5;
  request.run_id = "cost-run";
  request.idempotency_key = "cost-accepted";
  EXPECT_EQ(cost_manager.submit(request).state, WorkerJobState::Queued);
  adapter->submission_usage.cost_units = 0.6;
  request.idempotency_key = "cost-rejected";
  const auto cost_rejected = cost_manager.submit(request);
  EXPECT_EQ(cost_rejected.state, WorkerJobState::Failed);
  EXPECT_EQ(cost_rejected.failure_kind, WorkerFailureKind::Budget);
  EXPECT_EQ(cost_rejected.error, "worker cost_units budget exceeded");
}

TEST(Workers, ConcurrentCompletionUsageUpdatesAreSerializedForBudgets) {
  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  registry.add("usage", adapter);
  WorkerManager manager(*storage, registry, 32, 16, 16, 0, 10, 0.0);
  WorkerRequest request;
  request.worker_id = "usage";
  request.run_id = "concurrent-budget-run";
  request.node_id = "work";
  request.idempotency_key = "concurrent-budget-1";
  const auto first = manager.submit(request);
  request.idempotency_key = "concurrent-budget-2";
  const auto second = manager.submit(request);

  const auto completion = [](const std::string &job_id) {
    Event event;
    event.id = "completion-" + job_id;
    event.source_id = "worker.usage";
    event.type = "worker.job.completed";
    event.payload = {{"job_id", job_id},
                     {"external_job_id", "external-usage"},
                     {"result", {{"ok", true}}},
                     {"usage", {{"total_tokens", 6}}}};
    return event;
  };
  auto first_event = completion(first.id);
  auto second_event = completion(second.id);
  std::thread first_thread([&] { manager.receive(first_event); });
  std::thread second_thread([&] { manager.receive(second_event); });
  first_thread.join();
  second_thread.join();

  const auto first_after = manager.job(first.id);
  const auto second_after = manager.job(second.id);
  EXPECT_NE(first_after.state, second_after.state);
  EXPECT_TRUE(first_after.state == WorkerJobState::Completed ||
              second_after.state == WorkerJobState::Completed);
  EXPECT_TRUE(first_after.state == WorkerJobState::Failed ||
              second_after.state == WorkerJobState::Failed);
  const auto failed = first_after.state == WorkerJobState::Failed ? first_after : second_after;
  EXPECT_EQ(failed.failure_kind, WorkerFailureKind::Budget);
}

TEST(Workers, TransportAndJobFailuresRemainDistinguishable) {
  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  registry.add("usage", adapter);
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "usage";
  request.run_id = "failure-run";
  request.node_id = "work";
  request.idempotency_key = "transport";
  adapter->transport_failure = true;
  const auto transport = manager.submit(request);
  EXPECT_EQ(transport.state, WorkerJobState::Failed);
  EXPECT_EQ(transport.failure_kind, WorkerFailureKind::Transport);
  adapter->transport_failure = false;
  adapter->job_failure = true;
  request.idempotency_key = "job";
  const auto job = manager.submit(request);
  EXPECT_EQ(job.state, WorkerJobState::Failed);
  EXPECT_EQ(job.failure_kind, WorkerFailureKind::Job);
}
