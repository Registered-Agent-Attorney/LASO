#include "../support.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <laso/api/api.hpp>
#include <laso/workers/process_transport.hpp>
#include <memory>
#include <set>
#include <string>
#include <thread>

using namespace laso;
using namespace laso::test;

namespace {
ProcessWorkerConfig codex_config(const std::filesystem::path &root,
                                 const std::string &fixture_mode = "success",
                                 const std::string &worker_id = "codex") {
  ProcessWorkerConfig result;
  result.executable = LASO_CODEX_WORKER;
  result.args = {"--worker-id", worker_id, "--codex", LASO_CODEX_FIXTURE,
                 "--allowed-root", root.string(), "--timeout-ms", "2000"};
  if (fixture_mode != "success")
    result.environment["LASO_CODEX_FIXTURE_MODE"] = fixture_mode;
  result.startup_timeout_ms = 2000;
  result.request_timeout_ms = 2000;
  return result;
}

WorkerRequest request(const std::filesystem::path &root, const std::string &key,
                      const std::string &instructions, const std::string &session = {},
                      const std::string &worker_id = "codex") {
  WorkerRequest result;
  result.job_id = key;
  result.worker_id = worker_id;
  result.task_type = "coding";
  result.instructions = instructions;
  result.idempotency_key = key;
  result.run_id = "codex-run";
  result.node_id = "coding";
  result.metadata = {{"project_dir", root.string()}};
  if (!session.empty())
    result.metadata["codex_session_id"] = session;
  return result;
}

WorkerRequest durable_request(const std::filesystem::path &root, const std::string &key,
                              const std::string &instructions,
                              const std::optional<OpaqueProviderContinuation> &continuation = {},
                              const std::string &generation_id = {}) {
  auto result = request(root, key, instructions);
  result.durable_session = true;
  result.durable_session_id = "laso-session-test";
  result.continuation = continuation;
  if (!generation_id.empty())
    result.session_context = SessionContext{
        generation_id, "m6-test", "1", 1, 0, Json{{"summary", "M6 context marker"}}, Json::array()};
  return result;
}

Config codex_session_config(const std::filesystem::path &root,
                            const std::string &fixture_mode = "success") {
  auto result = config(root / "laso-state");
  ProcessWorkerConfig worker;
  worker.executable = LASO_CODEX_WORKER;
  worker.args = {"--codex",     LASO_CODEX_FIXTURE, "--allowed-root",
                 root.string(), "--timeout-ms",     "5000"};
  worker.startup_timeout_ms = 5000;
  worker.request_timeout_ms = 5000;
  if (fixture_mode != "success")
    worker.environment["LASO_CODEX_FIXTURE_MODE"] = fixture_mode;
  result.process_workers.emplace("codex", std::move(worker));
  return result;
}

Config real_codex_session_config(const std::filesystem::path &root) {
  auto result = codex_session_config(root);
  auto &worker = result.process_workers.at("codex");
  worker.args = {"--codex",        std::getenv("CODEX_BIN") ? std::getenv("CODEX_BIN") : "codex",
                 "--allowed-root", root.string(),
                 "--timeout-ms",   "180000"};
  worker.startup_timeout_ms = 30000;
  worker.request_timeout_ms = 180000;
  worker.interaction_timeout_ms = 300000;
  worker.environment_allowlist = {"HOME", "CODEX_HOME", "PATH"};
  result.session_context_reduction_enabled = true;
  result.session_context_reduction_target_bytes = 1024;
  result.session_context_reduction_threshold_bytes = 1200;
  result.session_context_reduction_max_input_bytes = 1024 * 1024;
  result.session_context_reduction_timeout_ms = 30000;
  result.validate();
  return result;
}

std::string session_worker_pipeline() {
  return "laso: '1'\nname: m6-codex-session\nversion: 1\n"
         "nodes:\n  agent:\n    type: worker\n    worker: codex\n"
         "    task_type: coding\n    instructions: Continue the LASO conversation.\n"
         "edges:\n  - {from: input, to: agent}\n  - {from: agent, to: output}\n";
}
} // namespace

TEST(CodexWorker, StructuredSessionFollowupAndUsage) {
  TemporaryDirectory root;
  ProcessWorkerTransport transport("codex", codex_config(root.path));
  ASSERT_NO_THROW(transport.start());
  ASSERT_TRUE(transport.metadata().healthy);
  const auto first = transport.submit(request(root.path, "codex-first", "make the fixture change"));
  ASSERT_EQ(first.state, WorkerJobState::Completed);
  ASSERT_EQ(first.result.value("session_id", ""), "fixture-session");
  ASSERT_EQ(first.result.value("summary", ""), "FIXTURE-COMPLETE");
  ASSERT_EQ(first.usage.input_tokens, std::optional<std::uint64_t>(11));
  ASSERT_EQ(first.usage.output_tokens, std::optional<std::uint64_t>(7));
  ASSERT_EQ(first.usage.executor, "codex");
  const auto second = transport.submit(
      request(root.path, "codex-second", "continue the existing session", "fixture-session"));
  EXPECT_EQ(second.state, WorkerJobState::Completed);
  EXPECT_EQ(second.result.value("summary", ""), "FIXTURE-CONTINUED");
  transport.stop();
}

TEST(CodexWorker, NonDurableSubmissionsStartIndependentCodexThreads) {
  TemporaryDirectory root;
  ProcessWorkerTransport transport("codex", codex_config(root.path));
  ASSERT_NO_THROW(transport.start());
  const auto first = transport.submit(request(root.path, "codex-independent-first", "first task"));
  const auto second =
      transport.submit(request(root.path, "codex-independent-second", "second task"));
  ASSERT_EQ(first.state, WorkerJobState::Completed) << first.error;
  ASSERT_EQ(second.state, WorkerJobState::Completed) << second.error;
  EXPECT_EQ(first.result.value("session_id", ""), "fixture-session");
  EXPECT_EQ(second.result.value("session_id", ""), "fixture-session-2");
  EXPECT_NE(first.result.value("session_id", ""), second.result.value("session_id", ""));
  transport.stop();
}

TEST(CodexWorker, ThreeIndependentWorkersOverlapAndReturnDistinctSessions) {
  TemporaryDirectory root;
  auto storage = make_storage(root.path / "state.db");
  WorkerRegistry registry;
  constexpr std::array<const char *, 3> agent_ids = {"agent-one", "agent-two", "agent-three"};
  constexpr std::array<const char *, 3> markers = {"MARKER-ONE", "MARKER-TWO", "MARKER-THREE"};
  std::array<std::shared_ptr<ProcessWorkerTransport>, agent_ids.size()> transports;
  std::array<std::filesystem::path, agent_ids.size()> started_markers;
  std::array<std::filesystem::path, agent_ids.size()> completed_markers;
  std::array<std::chrono::system_clock::time_point, agent_ids.size()> started_at{};
  std::array<std::chrono::system_clock::time_point, agent_ids.size()> completed_at{};
  std::array<std::chrono::steady_clock::time_point, agent_ids.size()> overlap_started_at{};
  std::array<std::chrono::steady_clock::time_point, agent_ids.size()> overlap_completed_at{};
  for (std::size_t index = 0; index < agent_ids.size(); ++index) {
    const auto id = std::string("codex-") + agent_ids[index];
    const auto workdir = root.path / id;
    std::filesystem::create_directories(workdir);
    auto worker = codex_config(root.path, "write-workspace-then-quiet", id);
    worker.request_timeout_ms = 8000;
    worker.environment["LASO_CODEX_FIXTURE_SLEEP_MS"] = "1200";
    worker.environment["LASO_CODEX_FIXTURE_SESSION_ID"] = id + "-provider-thread";
    worker.environment["LASO_CODEX_FIXTURE_OUTPUT"] = markers[index];
    started_markers[index] = root.path / (id + ".started");
    completed_markers[index] = root.path / (id + ".completed");
    worker.environment["LASO_CODEX_FIXTURE_MARKER"] = started_markers[index].string();
    worker.environment["LASO_CODEX_FIXTURE_DONE_MARKER"] = completed_markers[index].string();
    transports[index] = std::make_shared<ProcessWorkerTransport>(id, std::move(worker));
    ASSERT_NO_THROW(transports[index]->start());
    registry.add(id, transports[index]);
  }

  WorkerManager manager(*storage, registry, 8, 1);
  std::array<WorkerRequest, agent_ids.size()> requests;
  std::array<std::string, agent_ids.size()> job_ids;
  for (std::size_t index = 0; index < agent_ids.size(); ++index) {
    const auto id = std::string("codex-") + agent_ids[index];
    requests[index] = request(root.path / id, "three-agent-" + id, markers[index], {}, id);
    requests[index].run_id = "three-agent-acceptance";
    requests[index].node_id = agent_ids[index];
    (void)manager.submit_async(requests[index]);
    job_ids[index] = manager.job_id_for(requests[index].idempotency_key);
  }

  const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  bool all_started_before_any_completed = false;
  do {
    const auto observed_at = std::chrono::system_clock::now();
    const auto monotonic_at = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < agent_ids.size(); ++index) {
      if (started_at[index].time_since_epoch().count() == 0 &&
          std::filesystem::exists(started_markers[index])) {
        started_at[index] = observed_at;
        overlap_started_at[index] = monotonic_at;
      }
      if (completed_at[index].time_since_epoch().count() == 0 &&
          std::filesystem::exists(completed_markers[index])) {
        completed_at[index] = observed_at;
        overlap_completed_at[index] = monotonic_at;
      }
    }
    const bool all_started = std::all_of(started_markers.begin(), started_markers.end(),
                                         [](const auto &path) {
                                           return std::filesystem::exists(path);
                                         });
    const bool any_completed = std::any_of(completed_markers.begin(), completed_markers.end(),
                                           [](const auto &path) {
                                             return std::filesystem::exists(path);
                                           });
    if (all_started && !any_completed) {
      all_started_before_any_completed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < start_deadline);
  EXPECT_TRUE(all_started_before_any_completed)
      << "three Codex workers did not overlap before the first provider turn completed";

  const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool all_completed = false;
  do {
    const auto observed_at = std::chrono::system_clock::now();
    const auto monotonic_at = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < agent_ids.size(); ++index) {
      if (completed_at[index].time_since_epoch().count() == 0 &&
          std::filesystem::exists(completed_markers[index])) {
        completed_at[index] = observed_at;
        overlap_completed_at[index] = monotonic_at;
      }
    }
    all_completed = std::all_of(job_ids.begin(), job_ids.end(), [&](const auto &id) {
      return manager.job(id).state == WorkerJobState::Completed;
    });
    if (all_completed)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < completion_deadline);
  ASSERT_TRUE(all_completed) << "one or more independent Codex worker jobs failed to complete";

  std::set<std::string> provider_sessions;
  auto latest_start = std::chrono::steady_clock::time_point::min();
  auto earliest_completion = std::chrono::steady_clock::time_point::max();
  for (std::size_t index = 0; index < agent_ids.size(); ++index) {
    const auto completed = manager.job(job_ids[index]);
    EXPECT_EQ(completed.worker_id, std::string("codex-") + agent_ids[index]);
    EXPECT_EQ(completed.result.value("summary", ""), markers[index]);
    const auto provider_session = completed.result.value("session_id", "");
    ASSERT_FALSE(provider_session.empty());
    provider_sessions.insert(provider_session);
    const auto started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                started_at[index].time_since_epoch())
                                .count();
    const auto completed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  completed_at[index].time_since_epoch())
                                  .count();
    ASSERT_GT(started_ms, 0);
    ASSERT_NE(completed_at[index].time_since_epoch().count(), 0);
    ASSERT_NE(overlap_started_at[index].time_since_epoch().count(), 0);
    ASSERT_NE(overlap_completed_at[index].time_since_epoch().count(), 0);
    ASSERT_LT(overlap_started_at[index], overlap_completed_at[index]);
    latest_start = std::max(latest_start, overlap_started_at[index]);
    earliest_completion = std::min(earliest_completion, overlap_completed_at[index]);
    std::clog << "codex-isolation agent=" << agent_ids[index] << " worker=" << completed.worker_id
              << " job=" << completed.id << " provider_session=" << provider_session
              << " started_unix_ms=" << started_ms << " completed_unix_ms=" << completed_ms << '\n';
  }
  EXPECT_EQ(provider_sessions.size(), agent_ids.size());
  EXPECT_LT(latest_start, earliest_completion) << "provider execution intervals did not overlap";
  manager.stop();
  for (const auto &transport : transports)
    transport->stop();
}

TEST(CodexWorker, DurableSessionContinuationSurvivesAdapterRestartAndContextGeneration) {
  TemporaryDirectory root;
  std::string provider_session;
  std::optional<OpaqueProviderContinuation> continuation;
  {
    ProcessWorkerTransport transport("codex", codex_config(root.path));
    ASSERT_NO_THROW(transport.start());
    const auto first = transport.submit(
        durable_request(root.path, "codex-durable-first", "Remember the provided context."));
    ASSERT_EQ(first.state, WorkerJobState::Completed) << first.error;
    ASSERT_TRUE(first.continuation.has_value());
    EXPECT_EQ(first.continuation->provider_id, "codex");
    EXPECT_EQ(first.continuation->provider_version, "app-server");
    const auto state = Json::parse(first.continuation->state);
    provider_session = state.at("thread_id").get<std::string>();
    EXPECT_EQ(first.result.value("summary", ""), "FIXTURE-COMPLETE");
    EXPECT_FALSE(first.result.contains("session_id"));
    EXPECT_FALSE(first.metadata.contains("codex_session_id"));
    continuation = first.continuation;

    const auto second = transport.submit(durable_request(
        root.path, "codex-durable-second", "continue with the same context", continuation));
    ASSERT_EQ(second.state, WorkerJobState::Completed) << second.error;
    ASSERT_TRUE(second.continuation.has_value());
    EXPECT_EQ(second.continuation->state, continuation->state);
    EXPECT_EQ(second.result.value("summary", ""), "FIXTURE-CONTINUED");
    continuation = second.continuation;

    const auto generation = transport.submit(durable_request(root.path, "codex-durable-generation",
                                                             "continue after the context boundary",
                                                             continuation, "generation-2"));
    ASSERT_EQ(generation.state, WorkerJobState::Completed) << generation.error;
    ASSERT_TRUE(generation.continuation.has_value());
    const auto generation_state = Json::parse(generation.continuation->state);
    EXPECT_NE(generation_state.at("thread_id").get<std::string>(), provider_session);
    EXPECT_EQ(generation_state.at("context_generation_id"), "generation-2");
    EXPECT_EQ(generation.result.value("summary", ""), "FIXTURE-CONTEXT-SEEN");
    continuation = generation.continuation;
    provider_session = generation_state.at("thread_id").get<std::string>();
    transport.stop();
  }

  ProcessWorkerTransport restarted("codex", codex_config(root.path));
  ASSERT_NO_THROW(restarted.start());
  const auto recovered = restarted.submit(
      durable_request(root.path, "codex-durable-after-adapter-restart",
                      "continue after provider restart", continuation, "generation-2"));
  ASSERT_EQ(recovered.state, WorkerJobState::Completed) << recovered.error;
  ASSERT_TRUE(recovered.continuation.has_value());
  EXPECT_EQ(Json::parse(recovered.continuation->state).at("thread_id"), provider_session);
  EXPECT_EQ(recovered.result.value("summary", ""), "FIXTURE-CONTINUED");
  restarted.stop();
}

TEST(Sessions, CodexWorkerContinuesDurableTurnsAcrossLasoRestart) {
  TemporaryDirectory root;
  std::string session_id;
  std::string pipeline_id;
  std::string generation_id;
  std::string first_turn_id;
  {
    asio::io_context io;
    Service service(io, codex_session_config(root.path));
    pipeline_id = service.register_pipeline(session_worker_pipeline()).at("id").get<std::string>();
    const auto session = service.create_session(pipeline_id);
    session_id = session.id;
    const auto generation = service.create_session_context_generation(
        session_id, 0, 0, "m6-context-initial", "m6-fixture", "1",
        Json{{"summary", "M6 context marker"}});
    generation_id = generation.at("id").get<std::string>();
    const auto accepted =
        service.submit_session_turn(session_id, "m6-turn-one", Json{{"message", "Start"}});
    first_turn_id = accepted.at("id").get<std::string>();
    io.run();

    const auto first = service.get(RecordKind::SessionTurn, first_turn_id);
    ASSERT_EQ(first.at("state"), "succeeded");
    EXPECT_EQ(first.at("sequence"), 1);
    EXPECT_EQ(first.at("result").at("summary"), "FIXTURE-CONTEXT-SEEN");
    const auto jobs = service.worker_jobs(first.at("run_id").get<std::string>());
    ASSERT_EQ(jobs.size(), 1U);
    EXPECT_FALSE(jobs.front().contains("continuation"));
    const auto job_id = jobs.front().at("id").get<std::string>();
    LocalDevelopmentIdentity identity;
    Api api(service, identity);
    const auto public_job = api.handle("GET", "/api/v1/worker-jobs/" + job_id, "");
    ASSERT_EQ(public_job.status, 200U);
    EXPECT_EQ(public_job.body.dump().find("fixture-session"), std::string::npos);
    EXPECT_EQ(public_job.body.dump().find(root.path.string()), std::string::npos);
    const auto public_turn = api.handle("GET", "/api/v1/sessions/" + session_id + "/turns", "");
    ASSERT_EQ(public_turn.status, 200U);
    EXPECT_EQ(public_turn.body.dump().find("fixture-session"), std::string::npos);
    const auto events = api.handle("GET", "/api/v1/sessions/" + session_id + "/events?after=0", "");
    ASSERT_EQ(events.status, 200U);
    EXPECT_EQ(events.body.dump().find("fixture-session"), std::string::npos);
  }

  {
    asio::io_context io;
    Service restarted(io, codex_session_config(root.path));
    const auto second =
        restarted.submit_session_turn(session_id, "m6-turn-two", Json{{"message", "continue"}});
    io.run();
    const auto turn = restarted.get(RecordKind::SessionTurn, second.at("id").get<std::string>());
    ASSERT_EQ(turn.at("state"), "succeeded");
    EXPECT_EQ(turn.at("sequence"), 2);
    EXPECT_EQ(turn.at("result").at("summary"), "FIXTURE-CONTINUED");

    const auto generation = restarted.create_session_context_generation(
        session_id, 1, 2, "m6-context-after-two", "m6-fixture", "1",
        Json{{"summary", "M6 context marker"}});
    EXPECT_NE(generation.at("id").get<std::string>(), generation_id);
    const auto third =
        restarted.submit_session_turn(session_id, "m6-turn-three", Json{{"message", "continue"}});
    io.restart();
    io.run();
    const auto final_turn =
        restarted.get(RecordKind::SessionTurn, third.at("id").get<std::string>());
    ASSERT_EQ(final_turn.at("state"), "succeeded");
    EXPECT_EQ(final_turn.at("sequence"), 3);
    EXPECT_EQ(final_turn.at("result").at("summary"), "FIXTURE-CONTEXT-SEEN");
    EXPECT_EQ(restarted.list(RecordKind::SessionTurn, session_id).size(), 3U);
    const auto replay = restarted.session_events(session_id, 0, 100);
    EXPECT_TRUE(std::any_of(replay.begin(), replay.end(), [&](const Json &event) {
      return event.value("type", std::string{}) == "turn.execution.completed" &&
             event.value("turn_id", std::string{}) == first_turn_id;
    }));
  }
}

TEST(Sessions, CodexProviderDeathDoesNotAdvanceContinuationAndLaterTurnRecovers) {
  TemporaryDirectory root;
  std::string session_id;
  {
    asio::io_context io;
    Service service(io, codex_session_config(root.path, "crash"));
    const auto pipeline_id =
        service.register_pipeline(session_worker_pipeline()).at("id").get<std::string>();
    session_id = service.create_session(pipeline_id).id;
    const auto accepted = service.submit_session_turn(session_id, "m6-provider-crash-first",
                                                      Json{{"message", "start"}});
    io.run();
    const auto failed = service.get(RecordKind::SessionTurn, accepted.at("id").get<std::string>());
    ASSERT_EQ(failed.at("state"), "failed");
    EXPECT_TRUE(service.list(RecordKind::SessionContinuation, session_id).empty());
  }

  {
    asio::io_context io;
    Service restarted(io, codex_session_config(root.path));
    const auto accepted = restarted.submit_session_turn(session_id, "m6-provider-crash-recovery",
                                                        Json{{"message", "start after recovery"}});
    io.run();
    const auto recovered =
        restarted.get(RecordKind::SessionTurn, accepted.at("id").get<std::string>());
    ASSERT_EQ(recovered.at("state"), "succeeded");
    EXPECT_EQ(recovered.at("sequence"), 2);
    EXPECT_EQ(restarted.list(RecordKind::SessionContinuation, session_id).size(), 1U);
  }
}

TEST(Sessions, RealCodexDurableSessionContinuesAcrossRestartAndContextGeneration) {
  if (!std::getenv("LASO_RUN_REAL_CODEX"))
    GTEST_SKIP() << "Set LASO_RUN_REAL_CODEX=1 to run real Codex durable-session acceptance";

  TemporaryDirectory root;
  constexpr auto phrase = "M6-ORCHID-COPPER-31";
  std::ofstream(root.path / "m6-context.txt")
      << "The harmless project mnemonic is " << phrase << ".\n";

  std::string session_id;
  std::string first_turn_id;
  {
    asio::io_context io;
    Service service(io, real_codex_session_config(root.path));
    const auto pipeline_id =
        service.register_pipeline(session_worker_pipeline()).at("id").get<std::string>();
    const auto session = service.create_session(pipeline_id);
    session_id = session.id;

    const auto accepted = service.submit_session_turn(
        session_id, "m6-real-turn-one",
        Json{{"message", "Read m6-context.txt. Remember its harmless project mnemonic. "
                         "Reply with that exact mnemonic and do not modify any files."}});
    first_turn_id = accepted.at("id").get<std::string>();
    io.run();
    const auto first = service.get(RecordKind::SessionTurn, first_turn_id);
    ASSERT_EQ(first.at("state"), "succeeded");
    ASSERT_EQ(first.at("sequence"), 1);
    EXPECT_NE(first.at("result").value("summary", std::string{}).find(phrase), std::string::npos);
    const auto continuation = service.list(RecordKind::SessionContinuation, session_id);
    ASSERT_EQ(continuation.size(), 1U);
    EXPECT_EQ(continuation.front().value("provider_id", std::string{}), "codex");
    EXPECT_FALSE(service.latest_session_context_generation(session_id).has_value());
  }

  // A new Service also creates a new supervised Codex app-server process.
  // The durable continuation and PostgreSQL session state must be sufficient
  // to resume the same logical conversation without exposing native identity.
  {
    asio::io_context io;
    Service service(io, real_codex_session_config(root.path));
    const auto accepted = service.submit_session_turn(
        session_id, "m6-real-turn-two",
        Json{{"message", "Without rereading files or using tools, what exact mnemonic did I "
                         "ask you to remember? Reply with exactly that mnemonic."}});
    io.run();
    const auto second = service.get(RecordKind::SessionTurn, accepted.at("id").get<std::string>());
    ASSERT_EQ(second.at("state"), "succeeded");
    ASSERT_EQ(second.at("sequence"), 2);
    EXPECT_NE(second.at("result").value("summary", std::string{}).find(phrase), std::string::npos);
    EXPECT_FALSE(service.latest_session_context_generation(session_id).has_value());

    const auto third = service.submit_session_turn(
        session_id, "m6-real-turn-three",
        Json{{"message", "Using the established conversation context, repeat the exact mnemonic. "
                         "Do not read or modify files."}});
    io.restart();
    io.run();
    const auto final_turn = service.get(RecordKind::SessionTurn, third.at("id").get<std::string>());
    ASSERT_EQ(final_turn.at("state"), "succeeded");
    ASSERT_EQ(final_turn.at("sequence"), 3);
    EXPECT_NE(final_turn.at("result").value("summary", std::string{}).find(phrase),
              std::string::npos);

    const auto generation = service.latest_session_context_generation(session_id);
    ASSERT_TRUE(generation.has_value());
    EXPECT_EQ(generation->at("generation"), 1);
    EXPECT_EQ(generation->at("through_turn_sequence"), 2);
    EXPECT_NE(generation->at("payload").dump().find(phrase), std::string::npos);
    const auto snapshots =
        service.list(RecordKind::RunContextSnapshot, final_turn.at("run_id").get<std::string>());
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_EQ(snapshots.front().at("context_generation_id"), generation->at("id"));
    EXPECT_EQ(snapshots.front().at("context_through_turn_sequence"), 2);
    const auto continuations = service.list(RecordKind::SessionContinuation, session_id);
    ASSERT_EQ(continuations.size(), 1U);
    const auto native_state =
        Json::parse(continuations.front().at("state").get<std::string>()).at("thread_id");
    LocalDevelopmentIdentity identity;
    Api api(service, identity);
    const auto public_turns = api.handle("GET", "/api/v1/sessions/" + session_id + "/turns", "");
    ASSERT_EQ(public_turns.status, 200U);
    EXPECT_EQ(public_turns.body.dump().find(native_state.get<std::string>()), std::string::npos);
    const auto events = service.session_events(session_id, 0, 100);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [&](const Json &event) {
      return event.value("type", std::string{}) == "turn.execution.completed" &&
             event.value("turn_id", std::string{}) == first_turn_id;
    }));
  }
}

TEST(CodexWorker, DeterministicFixtureCanWriteIntoRequestedWorkspace) {
  TemporaryDirectory root;
  ProcessWorkerTransport transport("codex", codex_config(root.path, "write-workspace"));
  ASSERT_NO_THROW(transport.start());
  const auto submitted = transport.submit(
      request(root.path, "codex-workspace-artifact", "write the distributed fixture artifact"));
  ASSERT_EQ(submitted.state, WorkerJobState::Completed) << submitted.error;
  std::ifstream artifact(root.path / "remote-artifact.txt", std::ios_base::binary);
  const std::string contents((std::istreambuf_iterator<char>(artifact)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "cross-machine-s3-artifact-v1\n");
}

TEST(CodexWorker, SessionCanBeReconciledAfterAdapterRestart) {
  TemporaryDirectory root;
  std::string session;
  {
    ProcessWorkerTransport transport("codex", codex_config(root.path));
    ASSERT_NO_THROW(transport.start());
    const auto first = transport.submit(request(root.path, "codex-restart-first", "initial turn"));
    ASSERT_EQ(first.state, WorkerJobState::Completed);
    session = first.result.value("session_id", "");
  }
  ProcessWorkerTransport restarted("codex", codex_config(root.path));
  ASSERT_NO_THROW(restarted.start());
  const auto resumed =
      restarted.submit(request(root.path, "codex-restart-second", "continue", session));
  EXPECT_EQ(resumed.state, WorkerJobState::Completed);
  EXPECT_EQ(resumed.result.value("session_id", ""), session);
  restarted.stop();
}

TEST(CodexWorker, StartsNewSessionForDifferentWorkspaceRoot) {
  TemporaryDirectory parent;
  const auto first_root = parent.path / "first";
  const auto second_root = parent.path / "second";
  std::filesystem::create_directories(first_root);
  std::filesystem::create_directories(second_root);
  ProcessWorkerTransport transport("codex", codex_config(parent.path));
  ASSERT_NO_THROW(transport.start());
  const auto first = transport.submit(request(first_root, "codex-first-root", "first"));
  ASSERT_EQ(first.state, WorkerJobState::Completed);
  const auto second = transport.submit(request(second_root, "codex-second-root", "second"));
  EXPECT_EQ(second.state, WorkerJobState::Completed);
  EXPECT_EQ(first.result.value("project_dir", ""), first_root.string());
  EXPECT_EQ(second.result.value("project_dir", ""), second_root.string());
  transport.stop();
}

TEST(CodexWorker, QuietProviderIntervalUsesOverallDeadline) {
  TemporaryDirectory root;
  auto worker_config = codex_config(root.path, "quiet-over-one-minute");
  worker_config.args = {"--codex",          LASO_CODEX_FIXTURE, "--allowed-root",
                        root.path.string(), "--timeout-ms",     "65000"};
  worker_config.startup_timeout_ms = 2000;
  worker_config.request_timeout_ms = 65000;
  ProcessWorkerTransport transport("codex", worker_config);
  ASSERT_NO_THROW(transport.start());
  const auto result = transport.submit(request(root.path, "codex-quiet-provider", "continue"));
  EXPECT_EQ(result.state, WorkerJobState::Completed) << result.error;
  transport.stop();
}

TEST(CodexWorker, ProjectRootIsEnforced) {
  TemporaryDirectory root;
  TemporaryDirectory outside;
  ProcessWorkerTransport transport("codex", codex_config(root.path));
  ASSERT_NO_THROW(transport.start());
  const auto result = transport.submit(request(outside.path, "codex-outside", "do nothing"));
  EXPECT_EQ(result.state, WorkerJobState::Failed);
  EXPECT_NE(result.error.find("outside an allowed root"), std::string::npos);
}

TEST(CodexWorker, PermissionRequestUsesGenericWorkerChannel) {
  TemporaryDirectory root;
  const std::string worker_id = "codex-agent-test";
  ProcessWorkerTransport transport(worker_id, codex_config(root.path, "success", worker_id));
  ASSERT_NO_THROW(transport.start());
  EXPECT_EQ(transport.metadata().id, worker_id);
  unsigned requests = 0;
  transport.set_interaction_handler([&](const WorkerInteractionRequest &interaction) {
    ++requests;
    EXPECT_EQ(interaction.type, WorkerInteractionType::Permission);
    EXPECT_EQ(interaction.worker_id, worker_id);
    return WorkerInteractionResponse{interaction.request_id, WorkerInteractionState::Approved,
                                     Json{{"scope", "once"}}, "approved by test"};
  });
  const auto result =
      transport.submit(request(root.path, "codex-permission", "request-permission", {}, worker_id));
  EXPECT_EQ(result.state, WorkerJobState::Completed);
  EXPECT_EQ(requests, 1U);
}

TEST(CodexWorker, DurableSessionInteractionUsesLasoIdentity) {
  TemporaryDirectory root;
  ProcessWorkerTransport transport("codex", codex_config(root.path));
  unsigned requests = 0;
  const auto request_value =
      durable_request(root.path, "codex-durable-permission", "request-permission");
  transport.set_interaction_handler([&](const WorkerInteractionRequest &interaction) {
    ++requests;
    EXPECT_EQ(interaction.worker_job_id, request_value.job_id);
    EXPECT_EQ(interaction.external_job_id, request_value.job_id);
    EXPECT_EQ(interaction.session_id, request_value.durable_session_id);
    EXPECT_NE(interaction.session_id, "fixture-session");
    return WorkerInteractionResponse{interaction.request_id, WorkerInteractionState::Approved,
                                     Json{{"scope", "once"}}, "approved by test"};
  });
  ASSERT_NO_THROW(transport.start());
  const auto result = transport.submit(request_value);
  EXPECT_EQ(result.state, WorkerJobState::Completed) << result.error;
  EXPECT_EQ(requests, 1U);
}

TEST(CodexWorker, PermissionDenialRemainsAWorkerJobFailure) {
  TemporaryDirectory root;
  ProcessWorkerTransport transport("codex", codex_config(root.path));
  transport.set_interaction_handler([](const WorkerInteractionRequest &interaction) {
    EXPECT_EQ(interaction.type, WorkerInteractionType::Permission);
    return WorkerInteractionResponse{interaction.request_id, WorkerInteractionState::Denied,
                                     Json::object(), "denied by test"};
  });
  ASSERT_NO_THROW(transport.start());
  const auto result =
      transport.submit(request(root.path, "codex-permission-denied", "request-permission"));
  EXPECT_EQ(result.state, WorkerJobState::Failed);
  EXPECT_NE(result.error.find("permission denied"), std::string::npos);
}

TEST(CodexWorker, QuestionUsesGenericWorkerChannel) {
  TemporaryDirectory root;
  ProcessWorkerTransport transport("codex", codex_config(root.path));
  transport.set_interaction_handler([](const WorkerInteractionRequest &interaction) {
    EXPECT_EQ(interaction.type, WorkerInteractionType::Question);
    return WorkerInteractionResponse{interaction.request_id, WorkerInteractionState::Answered,
                                     Json{{"answers", Json{{"fixture-question", "yes"}}}},
                                     "answered by test"};
  });
  ASSERT_NO_THROW(transport.start());
  const auto result = transport.submit(request(root.path, "codex-question", "request-question"));
  EXPECT_EQ(result.state, WorkerJobState::Completed);
}

TEST(CodexWorker, MalformedAppServerMessagesAreTransportFailures) {
  TemporaryDirectory root;
  ProcessWorkerTransport transport("codex", codex_config(root.path, "malformed"));
  ASSERT_NO_THROW(transport.start());
  EXPECT_THROW(transport.submit(request(root.path, "codex-malformed", "run")),
               WorkerTransportError);
  EXPECT_FALSE(transport.metadata().healthy);
}

TEST(CodexWorker, RealInstalledCodexFixtureIsOptIn) {
  if (!std::getenv("LASO_RUN_REAL_CODEX"))
    GTEST_SKIP() << "Set LASO_RUN_REAL_CODEX=1 to run the configured Codex integration";
  TemporaryDirectory root;
  std::ofstream(root.path / "fixture.txt") << "before\n";
  auto worker_config = codex_config(root.path);
  worker_config.executable = LASO_CODEX_WORKER;
  worker_config.args = {
      "--codex",        std::getenv("CODEX_BIN") ? std::getenv("CODEX_BIN") : "codex",
      "--allowed-root", root.path.string(),
      "--timeout-ms",   "120000"};
  worker_config.startup_timeout_ms = 120000;
  worker_config.request_timeout_ms = 120000;
  worker_config.interaction_timeout_ms = 300000;
  worker_config.environment_allowlist = {"HOME", "CODEX_HOME", "PATH"};
  ProcessWorkerTransport transport("codex", worker_config);
  transport.set_interaction_handler([](const WorkerInteractionRequest &request) {
    return WorkerInteractionResponse{request.request_id, WorkerInteractionState::Approved,
                                     Json{{"scope", "once"}}, "approved by integration test"};
  });
  ASSERT_NO_THROW(transport.start());
  const auto first = transport.submit(request(root.path, "codex-real-first",
                                              "Change fixture.txt so it contains exactly "
                                              "CODEX-ADAPTER-OK and do not modify any other file. "
                                              "Reply with CODEX-DONE."));
  ASSERT_EQ(first.state, WorkerJobState::Completed) << first.error;
  const auto session = first.result.value("session_id", std::string{});
  ASSERT_FALSE(session.empty());
  std::ifstream changed(root.path / "fixture.txt");
  std::string contents;
  std::getline(changed, contents);
  EXPECT_EQ(contents, "CODEX-ADAPTER-OK");
  transport.stop();

  ProcessWorkerTransport restarted("codex", worker_config);
  restarted.set_interaction_handler([](const WorkerInteractionRequest &request) {
    return WorkerInteractionResponse{request.request_id, WorkerInteractionState::Approved,
                                     Json{{"scope", "once"}}, "approved by integration test"};
  });
  ASSERT_NO_THROW(restarted.start());
  const auto followup =
      restarted.submit(request(root.path, "codex-real-followup",
                               "Reply with CODEX-RECOVERED and do not edit files.", session));
  EXPECT_EQ(followup.state, WorkerJobState::Completed) << followup.error;
  EXPECT_EQ(followup.result.value("session_id", std::string{}), session);
  restarted.stop();
}
