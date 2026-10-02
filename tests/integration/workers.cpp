#include "../support.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <future>
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
    result.state = job_failure            ? WorkerJobState::Failed
                   : complete_immediately ? WorkerJobState::Completed
                                          : WorkerJobState::Queued;
    result.continuation = submission_continuation;
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
  std::optional<OpaqueProviderContinuation> submission_continuation;
  WorkerJobState status_state = WorkerJobState::Queued;
  Json status_result = nullptr;
  bool transport_failure = false, job_failure = false, complete_immediately = false;
};

class BrowserStatusWorker final : public WorkerTransport {
public:
  explicit BrowserStatusWorker(bool wait_for_cancel = false) : wait_for_cancel_(wait_for_cancel) {}
  WorkerMetadata metadata() const override {
    WorkerMetadata result;
    result.id = "windows_computer";
    result.name = result.id;
    result.capabilities = {"browser.status"};
    result.remote = true;
    result.enabled = true;
    result.healthy = true;
    result.status = "healthy";
    result.supports_status = true;
    result.supports_recovery = true;
    result.supports_cancellation = true;
    return result;
  }
  WorkerSubmission submit(const WorkerRequest &request) override {
    {
      std::lock_guard lock(mutex_);
      request_ = request;
      submitted_ = true;
    }
    submitted_changed_.notify_all();
    WorkerSubmission result;
    result.external_job_id = "computer-browser-status-job";
    result.state = WorkerJobState::Queued;
    return result;
  }
  WorkerStatus status(const std::string &) override {
    WorkerStatus result;
    if (cancelled_)
      result.state = WorkerJobState::Cancelled;
    else
      result.state = wait_for_cancel_ ? WorkerJobState::Running : WorkerJobState::Completed;
    return result;
  }
  WorkerStatus result(const std::string &) override {
    ++result_calls_;
    WorkerStatus result;
    result.state = cancelled_ ? WorkerJobState::Cancelled : WorkerJobState::Completed;
    if (!cancelled_)
      result.result = Json{{"window_count", 1},
                           {"browser_status", Json{{"browser_visible", true},
                                                    {"active_browser_visible", true}}}};
    return result;
  }
  bool cancel(const std::string &) override {
    cancelled_ = true;
    return true;
  }
  void start() override {}
  void stop() noexcept override {}
  bool wait_for_submission(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return submitted_changed_.wait_for(lock, timeout, [&] { return submitted_; });
  }
  WorkerRequest request() const {
    std::lock_guard lock(mutex_);
    return request_;
  }
  unsigned result_calls() const { return result_calls_; }

private:
  bool wait_for_cancel_;
  std::atomic<bool> cancelled_{false};
  std::atomic<unsigned> result_calls_{0};
  mutable std::mutex mutex_;
  std::condition_variable submitted_changed_;
  bool submitted_ = false;
  WorkerRequest request_;
};

class ParentAgentWorker final : public WorkerTransport {
public:
  WorkerMetadata metadata() const override {
    WorkerMetadata result;
    result.id = "agent-one";
    result.name = result.id;
    result.version = "app-server";
    result.capabilities = {"coding-agent"};
    result.enabled = true;
    result.healthy = true;
    result.status = "healthy";
    result.supports_cancellation = true;
    return result;
  }
  WorkerSubmission submit(const WorkerRequest &) override { return {}; }
  WorkerStatus status(const std::string &) override { return {}; }
  WorkerStatus result(const std::string &) override { return {}; }
  bool cancel(const std::string &) override { return true; }
  void start() override {}
  void stop() noexcept override {}
};

WorkerJob parent_codex_job(const std::string &id = "parent-codex-job") {
  WorkerJob result;
  result.id = id;
  result.worker_id = "agent-one";
  result.run_id = "browser-status-run";
  result.node_id = "agent";
  result.state = WorkerJobState::Running;
  result.request_metadata = {{"deadline", ""}, {"classification", "public"}};
  return result;
}

WorkerToolCallRequest browser_status_call(const std::string &parent_id = "parent-codex-job") {
  WorkerToolCallRequest result;
  result.request_id = "call-browser-status-1";
  result.worker_job_id = parent_id;
  result.worker_id = "agent-one";
  result.external_job_id = "codex:fixture-thread";
  result.session_id = "fixture-thread";
  result.turn_id = "fixture-turn";
  result.namespace_name = "laso";
  result.tool = "browser_status";
  return result;
}
} // namespace

TEST(Workers, OpaqueContinuationIsDurableButRedactedFromWorkerJobViews) {
  TemporaryDirectory directory;
  auto storage = make_storage(directory.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  adapter->submission_continuation =
      OpaqueProviderContinuation{"usage", "1", "opaque-provider-thread"};
  adapter->complete_immediately = true;
  registry.add("usage", adapter);
  PolicyEngine policy;
  WorkerManager manager(*storage, registry, policy);
  WorkerRequest request;
  request.worker_id = "usage";
  request.run_id = "run-with-continuation";
  request.idempotency_key = "durable-continuation";
  const auto submitted = manager.submit(request);
  ASSERT_TRUE(submitted.continuation.has_value());
  EXPECT_EQ(submitted.continuation->state, "opaque-provider-thread");
  const auto internal = manager.job(submitted.id);
  ASSERT_TRUE(internal.continuation.has_value());
  EXPECT_EQ(internal.continuation->state, "opaque-provider-thread");
  EXPECT_TRUE(
      storage->get(RecordKind::WorkerJob, submitted.id).contains("_continuation_candidate"));
  const auto public_jobs = manager.jobs(request.run_id);
  ASSERT_EQ(public_jobs.size(), 1U);
  EXPECT_FALSE(public_jobs.front().contains("_continuation_candidate"));
  WorkerManager restarted(*storage, registry, policy);
  ASSERT_TRUE(restarted.job(submitted.id).continuation.has_value());
  EXPECT_EQ(restarted.job(submitted.id).continuation->state, "opaque-provider-thread");
  EXPECT_EQ(restarted.continuation_candidate(submitted.id)->state, "opaque-provider-thread");

  adapter->job_failure = true;
  request.run_id = "failed-continuation-run";
  request.idempotency_key = "failed-continuation-key";
  const auto failed = manager.submit(request);
  EXPECT_EQ(failed.state, WorkerJobState::Failed);
  EXPECT_FALSE(storage->get(RecordKind::WorkerJob, failed.id).contains("_continuation_candidate"));
}

TEST(Workers, DistributedCapabilityAdvertisementContainsOnlySafeClaimData) {
  TemporaryDirectory directory;
  auto storage = make_storage(directory.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  registry.add("usage", adapter);
  PolicyEngine policy;
  WorkerManager manager(*storage, registry, policy);
  const auto advertised = manager.distributed_capabilities();
  EXPECT_EQ(advertised.at("protocol_version"), 1);
  ASSERT_EQ(advertised.at("workloads").size(), 1U);
  EXPECT_EQ(advertised.at("workloads").front().at("worker_id"), "usage");
  EXPECT_FALSE(advertised.dump().find("/home") != std::string::npos);
  EXPECT_TRUE(manager.can_execute("usage", ""));
  EXPECT_FALSE(manager.can_execute("missing", ""));
}

TEST(Workers, CodexBrowserStatusToolDispatchesFixedComputerJobAndPersistsResult) {
  TemporaryDirectory directory;
  auto storage = make_storage(directory.path / "state.db");
  WorkerRegistry registry;
  auto agent = std::make_shared<ParentAgentWorker>();
  auto computer = std::make_shared<BrowserStatusWorker>();
  registry.add("agent-one", agent);
  registry.add("windows_computer", computer);
  PolicyEngine policy({}, false, {"windows_computer"});
  WorkerManager manager(*storage, registry, policy);
  const auto parent = parent_codex_job();
  storage->commit({{RecordKind::WorkerJob, parent.id, parent.run_id, Json(parent)}});

  const auto request = browser_status_call();
  const auto response = manager.handle_tool_call(request);
  ASSERT_TRUE(response.success) << response.error;
  EXPECT_EQ(response.request_id, request.request_id);
  EXPECT_EQ(response.result.at("window_count"), 1);
  ASSERT_TRUE(computer->wait_for_submission(std::chrono::seconds(1)));

  const auto child_key = "codex-browser-status:" + parent.id + ":" + request.turn_id + ":" +
                         request.request_id;
  const auto child = manager.job(manager.job_id_for(child_key));
  EXPECT_EQ(child.state, WorkerJobState::Completed);
  EXPECT_EQ(child.worker_id, "windows_computer");
  EXPECT_EQ(child.run_id, parent.run_id);
  EXPECT_EQ(child.request_metadata.value("parent_worker_job_id", ""), parent.id);
  EXPECT_EQ(child.request_metadata.value("parent_tool_call_id", ""),
            request.turn_id + ":" + request.request_id);
  EXPECT_EQ(child.request_metadata.value("parent_codex_turn_id", ""), request.turn_id);
  EXPECT_EQ(child.request_metadata.value("parent_codex_session_id", ""), request.session_id);
  EXPECT_EQ(child.result, response.result);
  EXPECT_GE(computer->result_calls(), 1U);

  const auto dispatched = computer->request();
  EXPECT_EQ(dispatched.worker_id, "windows_computer");
  EXPECT_EQ(dispatched.capability, "browser.status");
  EXPECT_EQ(dispatched.task_type, "browser.status");
  EXPECT_EQ(dispatched.input, Json::object());
  EXPECT_EQ(dispatched.run_id, parent.run_id);
}

TEST(Workers, CodexBrowserStatusToolRejectsOtherToolsAndArguments) {
  TemporaryDirectory directory;
  auto storage = make_storage(directory.path / "state.db");
  WorkerRegistry registry;
  auto agent = std::make_shared<ParentAgentWorker>();
  auto computer = std::make_shared<BrowserStatusWorker>();
  registry.add("agent-one", agent);
  registry.add("windows_computer", computer);
  PolicyEngine policy({}, false, {"windows_computer"});
  WorkerManager manager(*storage, registry, policy);
  const auto parent = parent_codex_job();
  storage->commit({{RecordKind::WorkerJob, parent.id, parent.run_id, Json(parent)}});

  auto request = browser_status_call();
  request.tool = "command";
  const auto other_tool = manager.handle_tool_call(request);
  EXPECT_FALSE(other_tool.success);
  request = browser_status_call();
  request.arguments = {{"command", "must never be forwarded"}};
  const auto arguments = manager.handle_tool_call(request);
  EXPECT_FALSE(arguments.success);
  EXPECT_FALSE(computer->wait_for_submission(std::chrono::milliseconds(50)));
  EXPECT_EQ(manager.jobs(parent.run_id).size(), 1U);
}

TEST(Workers, CodexBrowserStatusToolFailsClosedWhenPolicyDeniesComputer) {
  TemporaryDirectory directory;
  auto storage = make_storage(directory.path / "state.db");
  WorkerRegistry registry;
  registry.add("agent-one", std::make_shared<ParentAgentWorker>());
  auto computer = std::make_shared<BrowserStatusWorker>();
  registry.add("windows_computer", computer);
  PolicyEngine policy({{"windows_computer", PolicyDecision::Deny}}, false,
                      {"windows_computer"});
  WorkerManager manager(*storage, registry, policy);
  const auto parent = parent_codex_job();
  storage->commit({{RecordKind::WorkerJob, parent.id, parent.run_id, Json(parent)}});

  const auto response = manager.handle_tool_call(browser_status_call());
  EXPECT_FALSE(response.success);
  EXPECT_NE(response.error.find("not allowed"), std::string::npos);
  EXPECT_FALSE(computer->wait_for_submission(std::chrono::milliseconds(50)));
  EXPECT_EQ(manager.jobs(parent.run_id).size(), 1U);
}

TEST(Workers, ParentCancellationCancelsDurableComputerToolChild) {
  TemporaryDirectory directory;
  auto storage = make_storage(directory.path / "state.db");
  WorkerRegistry registry;
  auto agent = std::make_shared<ParentAgentWorker>();
  auto computer = std::make_shared<BrowserStatusWorker>(true);
  registry.add("agent-one", agent);
  registry.add("windows_computer", computer);
  PolicyEngine policy({}, false, {"windows_computer"});
  WorkerManager manager(*storage, registry, policy);
  auto parent = parent_codex_job("parent-cancel-job");
  parent.external_job_id = "codex:fixture-thread";
  storage->commit({{RecordKind::WorkerJob, parent.id, parent.run_id, Json(parent)}});

  auto future = std::async(std::launch::async, [&] {
    return manager.handle_tool_call(browser_status_call(parent.id));
  });
  ASSERT_TRUE(computer->wait_for_submission(std::chrono::seconds(2)));
  const auto request = browser_status_call(parent.id);
  const auto child_key = "codex-browser-status:" + parent.id + ":" + request.turn_id + ":" +
                         request.request_id;
  const auto child_id = manager.job_id_for(child_key);
  const auto submit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (manager.job(child_id).external_job_id.empty() &&
         std::chrono::steady_clock::now() < submit_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  manager.cancel(parent.id, WorkerJobState::Cancelled, "acceptance cancellation");
  ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_FALSE(future.get().success);
  EXPECT_EQ(manager.job(parent.id).state, WorkerJobState::Cancelled);
  EXPECT_EQ(manager.job(child_id).state, WorkerJobState::Cancelled);
}

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

TEST(Workers, WorkerInteractionsAreDurableIdempotentAndCancellable) {
  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  PolicyEngine policy;
  WorkerManager manager(*storage, registry, policy);
  WorkerInteractionRequest request;
  request.request_id = "worker-request-1";
  request.worker_job_id = "job-1";
  request.worker_id = "worker";
  request.type = WorkerInteractionType::Question;
  request.title = "Need clarification";
  request.summary = "Choose a bounded answer";
  request.created_at = timestamp();
  request.deadline =
      format_utc_timestamp(parse_utc_timestamp(request.created_at) + std::chrono::seconds{10});
  request.payload = {{"choices", Json::array({"yes", "no"})}};
  std::optional<WorkerInteractionResponse> result;
  std::exception_ptr waiter_error;
  std::jthread waiter([&] {
    try {
      result = manager.handle_interaction(request);
    } catch (...) {
      waiter_error = std::current_exception();
    }
  });
  const auto persistence_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < persistence_deadline &&
         storage->list(RecordKind::WorkerInteraction).empty())
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (storage->list(RecordKind::WorkerInteraction).empty()) {
    waiter.join();
    EXPECT_FALSE(waiter_error);
    FAIL() << "worker interaction was not persisted within the bounded wait";
    return;
  }
  ASSERT_EQ(storage->list(RecordKind::WorkerInteraction).size(), 1U);
  EXPECT_EQ(storage->get(RecordKind::WorkerInteraction, request.request_id).at("state"), "pending");
  manager.resolve_interaction(request.request_id, WorkerInteractionState::Answered,
                              {{"answer", "yes"}}, "tester", "answered by test");
  waiter.join();
  EXPECT_FALSE(waiter_error);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->state, WorkerInteractionState::Answered);
  EXPECT_EQ(result->payload.at("answer"), "yes");
  EXPECT_EQ(manager.handle_interaction(request).state, WorkerInteractionState::Answered);
}

TEST(Workers, WorkerInteractionPolicyAndUnknownTypesAreExplicit) {
  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<UsageWorker>();
  registry.add("usage", adapter);
  PolicyEngine policy;
  WorkerManager manager(*storage, registry, policy);
  WorkerRequest job_request;
  job_request.worker_id = "usage";
  job_request.run_id = "interaction-policy";
  job_request.node_id = "work";
  job_request.idempotency_key = "interaction-policy-job";
  const auto job = manager.submit(job_request);

  WorkerInteractionRequest request;
  request.request_id = "permission-policy";
  request.worker_job_id = job.id;
  request.worker_id = "usage";
  request.type = WorkerInteractionType::Permission;
  request.title = "Run local test";
  request.summary = "The worker needs permission";
  request.payload = {{"resource", "local.test"}};
  const auto decision = manager.handle_interaction(request);
  EXPECT_EQ(decision.state, WorkerInteractionState::Approved);
  EXPECT_EQ(manager.worker_interaction(request.request_id).at("state"), "approved");

  EXPECT_THROW(Json({{"request_type", "unsupported"}}).get<WorkerInteractionRequest>(),
               std::invalid_argument);
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

TEST(Workers, PendingSubmissionCancellationIsAcknowledgedAndTerminal) {
  class BlockingWorker final : public WorkerAdapter {
  public:
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = "blocking";
      result.name = "blocking";
      result.enabled = true;
      result.healthy = true;
      result.status = "healthy";
      result.supports_cancellation = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      std::unique_lock lock(mutex);
      started = true;
      changed.notify_all();
      changed.wait(lock, [&] { return cancellation_requested; });
      throw WorkerTransportError("pending worker was cancelled");
    }
    WorkerStatus status(const std::string &) override {
      return {};
    }
    WorkerStatus result(const std::string &) override {
      return {};
    }
    bool cancel(const std::string &) override {
      return false;
    }
    bool cancel_pending(const std::string &) override {
      std::lock_guard lock(mutex);
      cancellation_requested = true;
      changed.notify_all();
      return true;
    }
    void start() override {}
    void stop() noexcept override {}

    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool cancellation_requested = false;
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<BlockingWorker>();
  registry.add("blocking", adapter);
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "blocking";
  request.run_id = "pending-cancel";
  request.node_id = "work";
  request.idempotency_key = "pending-cancel:work:1";

  std::promise<WorkerJob> completion;
  auto future = completion.get_future();
  std::thread submitter([&] {
    try {
      completion.set_value(manager.submit(request));
    } catch (...) {
      completion.set_exception(std::current_exception());
    }
  });
  {
    std::unique_lock lock(adapter->mutex);
    ASSERT_TRUE(
        adapter->changed.wait_for(lock, std::chrono::seconds(2), [&] { return adapter->started; }));
  }
  const auto jobs = manager.jobs(request.run_id);
  ASSERT_EQ(jobs.size(), 1U);
  manager.cancel(jobs.front().at("id"), WorkerJobState::Cancelled, "test cancellation");
  const auto cancelled = future.get();
  submitter.join();
  EXPECT_EQ(cancelled.state, WorkerJobState::Cancelled);
  EXPECT_TRUE(cancelled.cancellation_requested);
  EXPECT_TRUE(cancelled.cancellation_acknowledged);
}

TEST(Workers, AsyncSubmissionReturnsBeforeProviderHandleAndSupportsCancellation) {
  class BlockingWorker final : public WorkerAdapter {
  public:
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = "async-blocking";
      result.name = "async-blocking";
      result.enabled = true;
      result.healthy = true;
      result.status = "healthy";
      result.supports_recovery = true;
      result.supports_cancellation = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      std::unique_lock lock(mutex);
      started = true;
      changed.notify_all();
      changed.wait(lock, [&] { return cancellation_requested; });
      throw WorkerTransportError("pending worker was cancelled");
    }
    WorkerStatus status(const std::string &) override {
      status_called = true;
      return {};
    }
    WorkerStatus result(const std::string &) override {
      return {};
    }
    bool cancel(const std::string &) override {
      return false;
    }
    bool cancel_pending(const std::string &) override {
      std::lock_guard lock(mutex);
      cancellation_requested = true;
      changed.notify_all();
      return true;
    }
    void start() override {}
    void stop() noexcept override {}

    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool cancellation_requested = false;
    std::atomic<bool> status_called = false;
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<BlockingWorker>();
  registry.add("async-blocking", adapter);
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "async-blocking";
  request.run_id = "async-submit";
  request.node_id = "work";
  request.idempotency_key = "async-submit:work:1";

  const auto started_at = std::chrono::steady_clock::now();
  const auto submitted = manager.submit_async(request);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  started_at)
                .count(),
            500);
  EXPECT_EQ(submitted.id, manager.job_id_for(request.idempotency_key));
  EXPECT_EQ(submitted.state, WorkerJobState::Submitting);
  const auto refresh_started = std::chrono::steady_clock::now();
  const auto refreshed = manager.refresh(submitted.id);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  refresh_started)
                .count(),
            200);
  EXPECT_EQ(refreshed.state, WorkerJobState::Submitting);
  EXPECT_FALSE(adapter->status_called.load());
  {
    std::unique_lock lock(adapter->mutex);
    ASSERT_TRUE(
        adapter->changed.wait_for(lock, std::chrono::seconds(2), [&] { return adapter->started; }));
  }
  manager.cancel(submitted.id, WorkerJobState::Cancelled, "test async cancellation");
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    if (manager.job(submitted.id).state == WorkerJobState::Cancelled)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto cancelled = manager.job(submitted.id);
  EXPECT_EQ(cancelled.state, WorkerJobState::Cancelled);
  EXPECT_TRUE(cancelled.cancellation_requested);
  EXPECT_TRUE(cancelled.cancellation_acknowledged);
}

TEST(Workers, DifferentWorkerSubmissionsCanRunConcurrently) {
  struct SubmissionBarrier {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned started = 0;
    bool release = false;
  } barrier;

  class BarrierWorker final : public WorkerAdapter {
  public:
    BarrierWorker(SubmissionBarrier &barrier, std::string id)
        : barrier_(barrier), id_(std::move(id)) {}
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = id_;
      result.name = id_;
      result.status = "healthy";
      result.healthy = true;
      result.enabled = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      std::unique_lock lock(barrier_.mutex);
      ++barrier_.started;
      barrier_.changed.notify_all();
      barrier_.changed.wait(lock, [&] { return barrier_.release; });
      return {"external-" + id_, WorkerJobState::Queued, Json::object()};
    }
    WorkerStatus status(const std::string &) override {
      return {};
    }
    WorkerStatus result(const std::string &) override {
      return {};
    }
    bool cancel(const std::string &) override {
      return true;
    }
    void start() override {}
    void stop() noexcept override {}

  private:
    SubmissionBarrier &barrier_;
    std::string id_;
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  registry.add("parallel-one", std::make_shared<BarrierWorker>(barrier, "parallel-one"));
  registry.add("parallel-two", std::make_shared<BarrierWorker>(barrier, "parallel-two"));
  WorkerManager manager(*storage, registry, 4, 1);

  WorkerRequest first;
  first.worker_id = "parallel-one";
  first.run_id = "parallel-submit";
  first.node_id = "agent-one";
  first.idempotency_key = "parallel-submit:agent-one:1";
  WorkerRequest second = first;
  second.worker_id = "parallel-two";
  second.node_id = "agent-two";
  second.idempotency_key = "parallel-submit:agent-two:1";

  (void)manager.submit_async(first);
  (void)manager.submit_async(second);
  bool concurrent = false;
  {
    std::unique_lock lock(barrier.mutex);
    concurrent = barrier.changed.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return barrier.started == 2; });
    barrier.release = true;
    barrier.changed.notify_all();
  }
  EXPECT_TRUE(concurrent);
  const auto first_job_id = manager.job_id_for(first.idempotency_key);
  const auto second_job_id = manager.job_id_for(second.idempotency_key);
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    if (manager.job(first_job_id).state == WorkerJobState::Queued &&
        manager.job(second_job_id).state == WorkerJobState::Queued)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(manager.job(first_job_id).state, WorkerJobState::Queued);
  EXPECT_EQ(manager.job(second_job_id).state, WorkerJobState::Queued);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

TEST(Workers, UnknownJobsDoNotReserveGlobalOrWorkerCapacity) {
  const auto exercise = [](bool asynchronous) {
    TemporaryDirectory directory;
    auto storage = make_storage(directory.path / "state.db");
    WorkerRegistry registry;
    auto adapter = std::make_shared<UsageWorker>();
    registry.add("codex", adapter);

    WorkerJob stale;
    stale.id = "stale-computer-job";
    stale.worker_id = "windows-computer";
    stale.run_id = "stale-computer-run";
    stale.idempotency_key = "stale-computer-idempotency-key";
    stale.external_job_id = "lost-after-worker-restart";
    stale.state = WorkerJobState::Unknown;
    storage->commit({{RecordKind::WorkerJob, stale.id, stale.run_id, Json(stale)}});

    WorkerManager manager(*storage, registry, 1, 1);
    WorkerRequest request;
    request.worker_id = "codex";
    request.run_id = "independent-codex-run";
    request.node_id = "agent-one";
    request.idempotency_key = asynchronous ? "after-stale-async" : "after-stale-sync";
    WorkerJob submitted;
    if (asynchronous) {
      submitted = manager.submit_async(request);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      do {
        submitted = manager.job(submitted.id);
        if (submitted.state != WorkerJobState::Submitting)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      } while (std::chrono::steady_clock::now() < deadline);
    } else {
      submitted = manager.submit(request);
    }
    EXPECT_EQ(submitted.state, WorkerJobState::Queued);
    EXPECT_EQ(manager.job(stale.id).state, WorkerJobState::Unknown);
    manager.stop();
  };

  exercise(false);
  exercise(true);
}

TEST(Workers, ProviderCompletionWinsIfItBeatsCancellationAcknowledgement) {
  class CompletingWorker final : public WorkerAdapter {
  public:
    WorkerMetadata metadata() const override {
      WorkerMetadata result;
      result.id = "completing";
      result.name = "completing";
      result.enabled = true;
      result.healthy = true;
      result.status = "healthy";
      result.supports_cancellation = true;
      return result;
    }
    WorkerSubmission submit(const WorkerRequest &) override {
      std::unique_lock lock(mutex);
      started = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
      return {"external-completed",
              WorkerJobState::Completed,
              Json::object(),
              {},
              Json{{"winner", "provider"}}};
    }
    WorkerStatus status(const std::string &) override {
      return {};
    }
    WorkerStatus result(const std::string &) override {
      return {};
    }
    bool cancel(const std::string &) override {
      return false;
    }
    bool cancel_pending(const std::string &) override {
      return false;
    }
    void start() override {}
    void stop() noexcept override {}

    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool release = false;
  };

  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  WorkerRegistry registry;
  auto adapter = std::make_shared<CompletingWorker>();
  registry.add("completing", adapter);
  WorkerManager manager(*storage, registry);
  WorkerRequest request;
  request.worker_id = "completing";
  request.run_id = "completion-wins";
  request.node_id = "work";
  request.idempotency_key = "completion-wins:work:1";

  std::promise<WorkerJob> completion;
  auto future = completion.get_future();
  std::thread submitter([&] { completion.set_value(manager.submit(request)); });
  {
    std::unique_lock lock(adapter->mutex);
    ASSERT_TRUE(
        adapter->changed.wait_for(lock, std::chrono::seconds(2), [&] { return adapter->started; }));
  }
  const auto jobs = manager.jobs(request.run_id);
  ASSERT_EQ(jobs.size(), 1U);
  manager.cancel(jobs.front().at("id"), WorkerJobState::Cancelled, "late cancellation");
  {
    std::lock_guard lock(adapter->mutex);
    adapter->release = true;
  }
  adapter->changed.notify_all();
  const auto completed = future.get();
  submitter.join();
  EXPECT_EQ(completed.state, WorkerJobState::Completed);
  EXPECT_TRUE(completed.cancellation_requested);
  EXPECT_FALSE(completed.cancellation_acknowledged);
  EXPECT_EQ(completed.result.at("winner"), "provider");
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

TEST(Workers, RestartedSubmissionWithoutExternalIdIsUnknownAndNotResubmitted) {
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
      return {};
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
  request.run_id = "run-lost-submit";
  request.node_id = "work";
  request.idempotency_key = "run-lost-submit:work:1";
  WorkerManager restarted(*storage, registry);

  WorkerJob interrupted;
  interrupted.id = restarted.job_id_for(request.idempotency_key);
  interrupted.worker_id = request.worker_id;
  interrupted.run_id = request.run_id;
  interrupted.node_id = request.node_id;
  interrupted.idempotency_key = request.idempotency_key;
  interrupted.state = WorkerJobState::Submitting;
  ASSERT_TRUE(storage->claim(
      {RecordKind::WorkerJob, interrupted.id, interrupted.run_id, Json(interrupted)}));

  const auto recovered = restarted.submit_async(request);
  EXPECT_EQ(recovered.state, WorkerJobState::Unknown);
  EXPECT_TRUE(recovered.external_job_id.empty());
  EXPECT_EQ(adapter->submissions.load(), 0U);
  EXPECT_EQ(storage->get(RecordKind::WorkerJob, interrupted.id).get<WorkerJob>().state,
            WorkerJobState::Unknown);
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
