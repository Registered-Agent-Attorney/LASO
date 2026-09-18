#include "../support.hpp"
#include <array>
#include <atomic>
#include <boost/beast.hpp>
#include <fstream>
#include <laso/api/api.hpp>
#include <laso/workers/worker.hpp>
#include <thread>

using namespace laso;
using namespace laso::test;
TEST(Storage, PersistsAcrossConnections) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    {
      auto s = backend.open(dir.path / "state.db");
      s->commit({{RecordKind::Pipeline, "example", "", {{"value", 42}}}});
    }
    {
      auto s = backend.open(dir.path / "state.db");
      EXPECT_EQ(s->get(RecordKind::Pipeline, "example").at("value"), 42);
    }
  });
}
TEST(Storage, TransactionRollsBackWholeCheckpoint) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    std::vector<Record> batch{
        {RecordKind::Run, "first", "first", {{"valid", true}}},
        {RecordKind::Message, "large", "first", std::string(4 * 1024 * 1024 + 1, 'a')}};
    EXPECT_THROW(s->commit(batch), Error);
    EXPECT_THROW(s->get(RecordKind::Run, "first"), Error);
  });
}
TEST(Storage, ConformanceStoresAllRecordKinds) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    constexpr std::array kinds = {RecordKind::Pipeline,
                                  RecordKind::Run,
                                  RecordKind::Attempt,
                                  RecordKind::Message,
                                  RecordKind::Approval,
                                  RecordKind::Artifact,
                                  RecordKind::Event,
                                  RecordKind::Schedule,
                                  RecordKind::Trigger,
                                  RecordKind::ScheduleOccurrence,
                                  RecordKind::TriggerDelivery,
                                  RecordKind::EventSource,
                                  RecordKind::ExternalEventClaim,
                                  RecordKind::WorkerJob,
                                  RecordKind::WorkerInteraction};
    std::vector<Record> records;
    for (std::size_t i = 0; i < kinds.size(); ++i)
      records.push_back({kinds[i], "record-" + std::to_string(i), "run-1", {{"index", i}}});
    s->commit(records);
    for (std::size_t i = 0; i < kinds.size(); ++i) {
      EXPECT_EQ(s->get(kinds[i], "record-" + std::to_string(i)).at("index"), i);
      EXPECT_EQ(s->list(kinds[i], "run-1").size(), 1U);
    }
  });
}
TEST(Storage, ConformancePreservesOrderAcrossUpdates) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    s->commit({{RecordKind::Message, "first", "run-1", {{"value", 1}}},
               {RecordKind::Message, "second", "run-1", {{"value", 2}}}});
    s->commit({{RecordKind::Message, "first", "run-1", {{"value", 3}}}});
    const auto messages = s->list(RecordKind::Message, "run-1");
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].at("value"), 3);
    EXPECT_EQ(messages[1].at("value"), 2);
  });
}
TEST(Storage, ConformanceRejectsConflictingPipelineRevision) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    const Record original{RecordKind::Pipeline, "hello@1", "", {{"yaml", "one"}}};
    s->commit({original});
    EXPECT_NO_THROW(s->commit({original}));
    try {
      s->commit({{RecordKind::Pipeline, "hello@1", "", {{"yaml", "two"}}}});
      FAIL() << "conflicting pipeline revision should be rejected";
    } catch (const Error &error) {
      EXPECT_EQ(error.code, ErrorCode::Conflict);
    }
  });
}
TEST(Storage, ConformanceSupportsPagination) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    for (int i = 0; i < 3; ++i)
      s->commit({{RecordKind::Event, "event-" + std::to_string(i), "run-1", {{"index", i}}}});
    const auto page = s->list(RecordKind::Event, "run-1", 2, 1);
    ASSERT_EQ(page.size(), 2U);
    EXPECT_EQ(page[0].at("index"), 1);
    EXPECT_EQ(page[1].at("index"), 2);
  });
}
TEST(Storage, ConformancePaginatesBeyondOnePage) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    std::vector<Record> records;
    records.reserve(10001);
    for (unsigned i = 0; i < 10001; ++i)
      records.push_back({RecordKind::Event, "event-" + std::to_string(i), "run-1", {{"index", i}}});
    s->commit(records);
    const auto page = s->list(RecordKind::Event, "run-1", 1000, 10000);
    ASSERT_EQ(page.size(), 1U);
    EXPECT_EQ(page.front().at("index"), 10000U);
  });
}
TEST(Storage, ConformanceRejectsInvalidRecordInputs) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    try {
      s->commit({{static_cast<RecordKind>(99), "record", "run-1", Json::object()}});
      FAIL() << "invalid record kind should be rejected";
    } catch (const Error &error) {
      EXPECT_EQ(error.code, ErrorCode::Validation);
    }
    try {
      s->get(static_cast<RecordKind>(99), "record");
      FAIL() << "invalid get kind should be rejected";
    } catch (const Error &error) {
      EXPECT_EQ(error.code, ErrorCode::Validation);
    }
    try {
      s->list(static_cast<RecordKind>(99));
      FAIL() << "invalid list kind should be rejected";
    } catch (const Error &error) {
      EXPECT_EQ(error.code, ErrorCode::Validation);
    }
    EXPECT_THROW(s->commit({{RecordKind::Event, "", "run-1", Json::object()}}), Error);
    EXPECT_THROW(
        s->commit({{RecordKind::Event, "discarded", "run-1", Json(Json::value_t::discarded)}}),
        Error);
  });
}
TEST(Storage, ConformancePersistsStructuredOperationalRecords) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    const Json run = {{"id", "parent"},
                      {"state", "WaitingApproval"},
                      {"pipeline_id", "parent"},
                      {"pipeline_version", 2},
                      {"child_runs", Json::array({"child"})}};
    const Json child = {{"id", "child"},
                        {"parent_id", "parent"},
                        {"parent_node_id", "invoke"},
                        {"pipeline_id", "child"},
                        {"pipeline_version", 3}};
    const Json approval = {{"id", "approval"}, {"run_id", "child"}, {"decision", "pending"}};
    const Json artifact = {{"id", "artifact"}, {"run_id", "child"}, {"location", "local"}};
    const Json event = {{"id", "event"}, {"run_id", "child"}, {"type", "child.started"}};
    s->commit({{RecordKind::Run, "parent", "parent", run},
               {RecordKind::Run, "child", "parent", child},
               {RecordKind::Approval, "approval", "child", approval},
               {RecordKind::Artifact, "artifact", "child", artifact},
               {RecordKind::Event, "event", "child", event}});
    EXPECT_EQ(s->get(RecordKind::Run, "child"), child);
    EXPECT_EQ(s->list(RecordKind::Run, "parent").size(), 2U);
    EXPECT_EQ(s->get(RecordKind::Approval, "approval"), approval);
    EXPECT_EQ(s->get(RecordKind::Artifact, "artifact"), artifact);
    EXPECT_EQ(s->get(RecordKind::Event, "event"), event);
    auto decided = approval;
    decided["decision"] = "approved";
    s->commit({{RecordKind::Approval, "approval", "child", decided}});
    EXPECT_EQ(s->get(RecordKind::Approval, "approval").at("decision"), "approved");
    auto cancelled = run;
    cancelled["state"] = "Cancelled";
    s->commit({{RecordKind::Run, "parent", "parent", cancelled}});
    EXPECT_EQ(s->get(RecordKind::Run, "parent").at("state"), "Cancelled");
  });
}
TEST(Storage, ConformanceSerializesConcurrentCommits) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    std::atomic<bool> failed = false;
    std::vector<std::jthread> writers;
    for (unsigned writer = 0; writer < 4; ++writer) {
      writers.emplace_back([&, writer] {
        try {
          for (unsigned i = 0; i < 32; ++i) {
            const auto id = "concurrent-" + std::to_string(writer) + "-" + std::to_string(i);
            s->commit({{RecordKind::Event, id, "run-1", {{"writer", writer}, {"index", i}}}});
          }
        } catch (...) {
          failed = true;
        }
      });
    }
    writers.clear();
    EXPECT_FALSE(failed);
    EXPECT_EQ(s->list(RecordKind::Event, "run-1").size(), 128U);
  });
}
TEST(Storage, ConformanceClaimsDurableOccurrenceOnce) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    const Record first{RecordKind::ScheduleOccurrence,
                       "schedule|due",
                       "",
                       {{"status", "claimed"}, {"attempt", 1}}};
    EXPECT_TRUE(s->claim(first));
    EXPECT_FALSE(s->claim(
        {RecordKind::ScheduleOccurrence, first.id, "", {{"status", "claimed"}, {"attempt", 2}}}));
    EXPECT_EQ(s->get(RecordKind::ScheduleOccurrence, first.id).at("attempt"), 1);
    EXPECT_TRUE(
        s->claim({RecordKind::TriggerDelivery, "trigger|event", "", {{"status", "claimed"}}}));
  });
}
TEST(Storage, ConformanceSerializesConcurrentClaims) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    std::atomic<unsigned> winners = 0;
    std::vector<std::jthread> claimers;
    for (unsigned i = 0; i < 8; ++i) {
      claimers.emplace_back([&, i] {
        if (s->claim({RecordKind::ScheduleOccurrence, "same-occurrence", "", {{"claimer", i}}}))
          ++winners;
      });
    }
    claimers.clear();
    EXPECT_EQ(winners, 1U);
  });
}
TEST(Storage, ConformanceAtomicallyClaimsExternalEventAndDeduplicates) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    const Record claim{
        RecordKind::ExternalEventClaim,
        "external-event:source:event-1",
        "",
        {{"source_id", "source"}, {"external_event_id", "event-1"}, {"event_id", "event-record"}}};
    const Record event{RecordKind::Event, "event-record", "", {{"type", "example.created"}}};
    EXPECT_TRUE(s->claim(claim, {event}));
    EXPECT_FALSE(s->claim(claim, {{RecordKind::Event, "other-event", "", Json::object()}}));
    EXPECT_EQ(s->list(RecordKind::Event).size(), 1U);
    EXPECT_EQ(s->get(RecordKind::ExternalEventClaim, claim.id).at("event_id"), "event-record");
  });
}
TEST(Storage, ConformanceSerializesConcurrentExternalEventClaims) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    std::atomic<unsigned> winners = 0;
    std::vector<std::jthread> claimers;
    for (unsigned i = 0; i < 8; ++i) {
      claimers.emplace_back([&, i] {
        const auto event_id = "external-event-" + std::to_string(i);
        if (s->claim({RecordKind::ExternalEventClaim,
                      "external-event:source:same",
                      "",
                      {{"event_id", event_id}}},
                     {{RecordKind::Event, event_id, "", {{"type", "example.created"}}}}))
          ++winners;
      });
    }
    claimers.clear();
    EXPECT_EQ(winners, 1U);
    EXPECT_EQ(s->list(RecordKind::Event).size(), 1U);
  });
}

TEST(Storage, ConformancePersistsWorkerJobLifecycleAndRejectsInvalidUpdates) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    WorkerJob job;
    job.id = "worker-job-1";
    job.worker_id = "offline";
    job.run_id = "run-1";
    job.node_id = "work";
    job.idempotency_key = "run-1:work:1";
    s->commit({{RecordKind::WorkerJob, job.id, job.run_id, Json(job)}});

    job.state = WorkerJobState::Submitting;
    s->commit({{RecordKind::WorkerJob, job.id, job.run_id, Json(job)}});
    job.state = WorkerJobState::Queued;
    s->commit({{RecordKind::WorkerJob, job.id, job.run_id, Json(job)}});
    job.state = WorkerJobState::Running;
    job.external_job_id = "external-1";
    s->commit({{RecordKind::WorkerJob, job.id, job.run_id, Json(job)}});
    job.state = WorkerJobState::Completed;
    job.result = Json{{"ok", true}};
    job.completed_at = timestamp();
    s->commit({{RecordKind::WorkerJob, job.id, job.run_id, Json(job)}});
    EXPECT_EQ(s->get(RecordKind::WorkerJob, job.id).template get<WorkerJob>().result.at("ok"),
              true);

    auto late = job;
    late.state = WorkerJobState::Failed;
    EXPECT_THROW(s->commit({{RecordKind::WorkerJob, late.id, late.run_id, Json(late)}}), Error);

    WorkerJob invalid;
    invalid.id = "worker-job-invalid";
    invalid.worker_id = "offline";
    invalid.run_id = "run-1";
    invalid.node_id = "work";
    invalid.idempotency_key = "run-1:work:2";
    invalid.state = WorkerJobState::Created;
    s->commit({{RecordKind::WorkerJob, invalid.id, invalid.run_id, Json(invalid)}});
    invalid.state = WorkerJobState::Completed;
    EXPECT_THROW(s->commit({{RecordKind::WorkerJob, invalid.id, invalid.run_id, Json(invalid)}}),
                 Error);

    WorkerJob retry = invalid;
    retry.id = "worker-job-retry";
    retry.state = WorkerJobState::Created;
    retry.attempt = 2;
    retry.idempotency_key = "run-1:work:2";
    s->commit({{RecordKind::WorkerJob, retry.id, retry.run_id, Json(retry)}});
    EXPECT_EQ(s->list(RecordKind::WorkerJob, "run-1").size(), 3U);
  });
}

TEST(Storage, ConformanceClaimsWorkerJobIdentityOnce) {
  for_each_storage_backend([](const auto &backend) {
    TemporaryDirectory dir;
    auto s = backend.open(dir.path / "state.db");
    WorkerJob job;
    job.id = "worker-claim";
    job.worker_id = "offline";
    job.run_id = "run-claim";
    job.node_id = "work";
    job.idempotency_key = "run-claim:work:1";
    EXPECT_TRUE(s->claim({RecordKind::WorkerJob, job.id, job.run_id, Json(job)}));
    EXPECT_FALSE(s->claim({RecordKind::WorkerJob, job.id, job.run_id, Json(job)}));
    EXPECT_EQ(s->list(RecordKind::WorkerJob, job.run_id).size(), 1U);
  });
}

TEST(Storage, PostgresRejectsSecondOwner) {
#if defined(LASO_HAS_POSTGRES)
  if (!std::getenv("LASO_TEST_POSTGRES_DSN"))
    GTEST_SKIP() << "LASO_TEST_POSTGRES_DSN is not configured";
  for (const auto &backend : storage_backends()) {
    if (backend.name != "postgres")
      continue;
    TemporaryDirectory dir;
    auto first = backend.open(dir.path / "state.db");
    try {
      auto second = backend.open(dir.path / "state.db");
      (void)second;
      ADD_FAILURE() << "a second PostgreSQL storage owner was accepted";
    } catch (const Error &error) {
      EXPECT_EQ(error.code, ErrorCode::Conflict);
      EXPECT_STREQ(error.what(), "PostgreSQL database is owned by another LASO process");
    }
  }
#else
  GTEST_SKIP() << "PostgreSQL backend is not enabled";
#endif
}
TEST(Storage, PostgresRunsAndRecoversNormalRuntime) {
#if defined(LASO_HAS_POSTGRES)
  const auto *dsn = std::getenv("LASO_TEST_POSTGRES_DSN");
  if (!dsn || !*dsn)
    GTEST_SKIP() << "LASO_TEST_POSTGRES_DSN is not configured";
  const auto dsn_copy = std::string(dsn);
  auto schema = "laso_runtime_" + uuid();
  std::replace(schema.begin(), schema.end(), '-', '_');
  TemporaryDirectory dir;
  Config c = config(dir.path);
  c.storage_backend = "postgres";
  c.postgres_dsn = dsn_copy;
  c.postgres_schema = schema;
  c.validate();
  std::string run_id;
  try {
    {
      asio::io_context io;
      Service service(io, c);
      service.register_pipeline(fixture("hello-pipeline"));
      const auto parent = service.register_pipeline(fixture("subpipeline"));
      run_id = service.start(parent.at("id").get<std::string>(), Json{{"value", 42}});
      io.run();
      const auto run = service.get(RecordKind::Run, run_id).get<laso::Run>();
      EXPECT_EQ(run.state, RunState::Completed);
      EXPECT_EQ(run.child_runs.size(), 1U);
      EXPECT_EQ(service.get(RecordKind::Run, run.child_runs.front()).at("pipeline_version"), 1U);
    }
    {
      asio::io_context io;
      Service reopened(io, c);
      const auto run = reopened.get(RecordKind::Run, run_id).get<laso::Run>();
      EXPECT_EQ(run.state, RunState::Completed);
      EXPECT_EQ(reopened.run_view(run_id).at("children").size(), 1U);
    }
  } catch (...) {
    pqxx::connection connection(dsn_copy);
    pqxx::work transaction(connection);
    transaction.exec("DROP SCHEMA IF EXISTS \"" + schema + "\" CASCADE");
    transaction.commit();
    throw;
  }
  pqxx::connection connection(dsn_copy);
  pqxx::work transaction(connection);
  transaction.exec("DROP SCHEMA IF EXISTS \"" + schema + "\" CASCADE");
  transaction.commit();
#else
  GTEST_SKIP() << "PostgreSQL backend is not enabled";
#endif
}
TEST(Storage, ProcessLeasePreventsCompetingExecutors) {
  TemporaryDirectory dir;
  ProcessLease first(dir.path / "state.db");
  EXPECT_THROW(ProcessLease(dir.path / "state.db"), Error);
}
TEST(Artifacts, IgnoresUntrustedNamesForPath) {
  TemporaryDirectory dir;
  auto storage = make_storage(dir.path / "state.db");
  LocalArtifactStore artifacts(dir.path / "artifacts", *storage);
  Artifact a;
  a.name = "../../outside";
  a.run_id = "../../outside";
  std::string data = "example";
  auto saved = artifacts.put(a, std::as_bytes(std::span(data.data(), data.size())));
  EXPECT_EQ(std::filesystem::path(saved.location).parent_path(), dir.path / "artifacts");
  EXPECT_TRUE(std::filesystem::exists(saved.location));
  EXPECT_EQ(storage->get(RecordKind::Artifact, saved.id).at("name"), "../../outside");
}
TEST(Plugins, DiscoversLoadsInvokesAndUnloadsExample) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto c = config(dir.path);
  c.plugin_dirs = {LASO_PLUGIN_DIR};
  Service s(io, c);
  ASSERT_EQ(s.plugins().size(), 3U);
  EXPECT_TRUE(s.plugins().at(0).at("loaded").get<bool>());
  auto r = execute(s, io, fixture("native-plugin"), {{"echo", 123}});
  EXPECT_EQ(r.state, RunState::Completed);
  EXPECT_EQ(r.message.payload.at("echo"), 123);
}
TEST(Plugins, ModelProviderIsDiscoverableAndServicesAgentPipeline) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto c = config(dir.path);
  c.plugin_dirs = {LASO_PLUGIN_DIR};
  c.models["plugin-model"] = {"example-model", "offline-example"};
  Service s(io, c);
  const auto providers = s.providers();
  const auto found = std::find_if(providers.begin(), providers.end(), [](const Json &provider) {
    return provider.at("name") == "example-model" &&
           provider.at("plugin") == "example-model-provider" && provider.at("healthy") == true;
  });
  ASSERT_NE(found, providers.end());
  auto r = execute(s, io, R"(laso: "1"
name: plugin-model
version: 1
nodes:
  generate:
    type: agent
    model: plugin-model
    prompt: Produce a deterministic offline response.
edges:
  - {from: input, to: generate}
  - {from: generate, to: output}
)",
                   Json{{"request", "example"}});
  EXPECT_EQ(r.state, RunState::Completed);
  EXPECT_EQ(r.message.payload.at("text"), "Offline plugin model response");
  EXPECT_TRUE(r.message.payload.at("reviewed"));
  const auto provenance = std::find_if(
      r.message.provenance.begin(), r.message.provenance.end(), [](const ProvenanceRecord &item) {
        return item.provider == "example-model" && item.model == "offline-example";
      });
  EXPECT_NE(provenance, r.message.provenance.end());
}
TEST(Plugins, RejectsIncompatibleABI) {
  ToolRegistry r;
  ProviderRegistry providers;
  PluginLoader loader(r, providers);
  loader.discover({LASO_BAD_PLUGIN_DIR});
  ASSERT_EQ(loader.plugins().size(), 1U);
  EXPECT_FALSE(loader.plugins().front().loaded);
  EXPECT_EQ(loader.plugins().front().abi, 999U);
  EXPECT_TRUE(r.names().empty());
}
TEST(Plugins, RejectsNonLibraryFile) {
  TemporaryDirectory dir;
  std::ofstream(dir.path / "bad.so") << "not a library";
  ToolRegistry r;
  ProviderRegistry providers;
  PluginLoader loader(r, providers);
  loader.discover({dir.path});
  ASSERT_EQ(loader.plugins().size(), 1U);
  EXPECT_FALSE(loader.plugins().front().loaded);
}
TEST(Plugins, NoImplicitDirectories) {
  ToolRegistry r;
  ProviderRegistry providers;
  PluginLoader loader(r, providers);
  loader.discover({});
  EXPECT_TRUE(loader.plugins().empty());
}
TEST(Plugins, MissingConfiguredDirectoryFailsClearly) {
  ToolRegistry tools;
  ProviderRegistry providers;
  PluginLoader loader(tools, providers);
  try {
    loader.discover({std::filesystem::temp_directory_path() / "laso-no-such-plugin-dir"});
    FAIL() << "missing plugin directory should fail configuration";
  } catch (const Error &error) {
    EXPECT_EQ(error.code, ErrorCode::Configuration);
    EXPECT_STREQ(error.what(), "Configured plugin directory is unavailable");
  }
}
TEST(Plugins, SymlinksNotLoaded) {
  TemporaryDirectory dir;
  std::filesystem::create_symlink(
      std::filesystem::path(LASO_PLUGIN_DIR) / "liblaso_example_tool.so", dir.path / "redirect.so");
  ToolRegistry r;
  ProviderRegistry providers;
  PluginLoader loader(r, providers);
  loader.discover({dir.path});
  EXPECT_TRUE(loader.plugins().empty());
}
TEST(Api, HealthAndVersion) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service s(io, config(dir.path));
  LocalDevelopmentIdentity identity;
  Api api(s, identity);
  EXPECT_EQ(api.handle("GET", "/api/v1/health", "").status, 200U);
  EXPECT_EQ(api.handle("GET", "/api/v1/version", "").body.at("version"), "0.1.0");
}
TEST(Api, RegistersAndCreatesRun) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service s(io, config(dir.path));
  LocalDevelopmentIdentity identity;
  Api api(s, identity);
  EXPECT_EQ(
      api.handle("POST", "/api/v1/pipelines", Json{{"yaml", fixture("hello-pipeline")}}.dump())
          .status,
      201U);
  auto response = api.handle("POST", "/api/v1/pipelines/hello/runs", "{}");
  ASSERT_EQ(response.status, 202U);
  io.run();
  auto id = response.body.at("id").get<std::string>();
  EXPECT_EQ(api.handle("GET", "/api/v1/runs/" + id, "").body.at("state"), "Completed");
}
TEST(Api, RunMetadataIsPreservedForWorkerContext) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service s(io, config(dir.path));
  LocalDevelopmentIdentity identity;
  Api api(s, identity);
  ASSERT_EQ(
      api.handle("POST", "/api/v1/pipelines", Json{{"yaml", fixture("hello-pipeline")}}.dump())
          .status,
      201U);
  const auto response = api.handle(
      "POST", "/api/v1/pipelines/hello/runs",
      Json{{"input", Json{{"value", 42}}},
           {"metadata", Json{{"classification", "public"}, {"project_dir", "/tmp/example"}}}}
          .dump());
  ASSERT_EQ(response.status, 202U);
  io.run();
  const auto run =
      api.handle("GET", "/api/v1/runs/" + response.body.at("id").get<std::string>(), "").body;
  EXPECT_EQ(run.at("message").at("metadata").at("classification"), "public");
  EXPECT_EQ(run.at("message").at("metadata").at("project_dir"), "/tmp/example");
}
TEST(Api, RejectsMalformedAndOversizedRequests) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service s(io, config(dir.path));
  LocalDevelopmentIdentity identity;
  Api api(s, identity);
  EXPECT_EQ(api.handle("POST", "/api/v1/pipelines", "{").status, 400U);
  EXPECT_EQ(api.handle("POST", "/api/v1/pipelines", std::string(1024 * 1024 + 1, 'a')).status,
            413U);
}
TEST(Api, AuthenticationBoundaryApplies) {
  struct Denied : IdentityProvider {
    Actor authenticate(const std::string &) const override {
      return {};
    }
    bool authorize(const AuthorizationContext &) const override {
      return false;
    }
  } identity;
  TemporaryDirectory dir;
  asio::io_context io;
  Service s(io, config(dir.path));
  Api api(s, identity);
  EXPECT_EQ(api.handle("GET", "/api/v1/runs", "").status, 403U);
}
TEST(Api, ServesRealHTTPHealth) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service s(io, config(dir.path));
  LocalDevelopmentIdentity identity;
  Api api(s, identity);
  HttpServer server(io, api, "127.0.0.1", 0);
  auto port = server.port();
  server.start();
  std::jthread worker([&] { io.run(); });
  asio::io_context peer_io;
  boost::beast::tcp_stream stream(peer_io);
  stream.expires_after(std::chrono::seconds(5));
  stream.connect({asio::ip::make_address("127.0.0.1"), port});
  boost::beast::http::request<boost::beast::http::empty_body> request{boost::beast::http::verb::get,
                                                                      "/api/v1/health", 11};
  request.set(boost::beast::http::field::host, "localhost");
  boost::beast::http::write(stream, request);
  boost::beast::flat_buffer buffer;
  boost::beast::http::response<boost::beast::http::string_body> response;
  boost::beast::http::read(stream, buffer, response);
  EXPECT_EQ(response.result_int(), 200U);
  EXPECT_EQ(Json::parse(response.body()).at("status"), "ok");
  server.stop();
  worker.join();
}
TEST(Scheduler, FiniteIntervalSchedule) {
  asio::io_context io;
  unsigned count = 0;
  LocalScheduler scheduler(io, [&](const ScheduledPipeline &) { ++count; });
  ScheduledPipeline s;
  s.max_firings = 3;
  s.interval = Milliseconds{1};
  scheduler.schedule(s);
  io.run();
  EXPECT_EQ(count, 3U);
}
TEST(Scheduler, StopBeforeExecutorStarts) {
  asio::io_context io;
  unsigned count = 0;
  LocalScheduler scheduler(io, [&](const ScheduledPipeline &) { ++count; });
  scheduler.schedule({});
  scheduler.stop();
  io.run();
  EXPECT_EQ(count, 0U);
}
TEST(Storage, InterruptedRunBecomesInspectablePausedCheckpoint) {
  TemporaryDirectory dir;
  auto c = config(dir.path);
  laso::Run r;
  r.state = RunState::Running;
  r.pipeline_id = "hello";
  r.definition = fixture("hello-pipeline");
  {
    auto s = make_storage(c.db_path);
    s->commit({{RecordKind::Run, r.id, r.id, Json(r)}});
  }
  asio::io_context io;
  Service s(io, c);
  EXPECT_EQ(s.get(RecordKind::Run, r.id).get<laso::Run>().state, RunState::Paused);
}
TEST(Storage, PersistedCancellationSurvivesRestart) {
  TemporaryDirectory dir;
  auto c = config(dir.path);
  laso::Run r;
  r.state = RunState::Running;
  r.cancellation_requested = true;
  {
    auto s = make_storage(c.db_path);
    s->commit({{RecordKind::Run, r.id, r.id, Json(r)}});
  }
  asio::io_context io;
  Service s(io, c);
  EXPECT_EQ(s.get(RecordKind::Run, r.id).get<laso::Run>().state, RunState::Cancelled);
}
TEST(Storage, RecoveryDoesNotRewriteTerminalRuns) {
  TemporaryDirectory dir;
  auto c = config(dir.path);
  laso::Run r;
  r.state = RunState::Completed;
  r.cancellation_requested = true;
  r.pipeline_id = "completed";
  r.definition = fixture("hello-pipeline");
  {
    auto storage = make_storage(c.db_path);
    storage->commit({{RecordKind::Run, r.id, r.id, Json(r)}});
  }
  asio::io_context io;
  Service service(io, c);
  EXPECT_EQ(service.get(RecordKind::Run, r.id).get<laso::Run>().state, RunState::Completed);
}
TEST(Storage, RecoveryScansAttemptsBeyondOnePage) {
  TemporaryDirectory dir;
  auto c = config(dir.path);
  laso::Run r;
  r.state = RunState::Running;
  r.pipeline_id = "recovery";
  r.definition = fixture("hello-pipeline");
  std::vector<Record> records{{RecordKind::Run, r.id, r.id, Json(r)}};
  std::string interrupted;
  for (unsigned i = 0; i < 10001; ++i) {
    NodeExecution attempt;
    attempt.run_id = r.id;
    attempt.node_id = "action";
    attempt.state = i == 10000 ? NodeState::Running : NodeState::Completed;
    if (i == 10000)
      interrupted = attempt.id;
    records.push_back({RecordKind::Attempt, attempt.id, r.id, Json(attempt)});
  }
  {
    auto storage = make_storage(c.db_path);
    storage->commit(records);
  }
  asio::io_context io;
  Service service(io, c);
  EXPECT_EQ(service.get(RecordKind::Attempt, interrupted).at("state"), "Failed");
}
TEST(Api, PaginationBounds) {
  TemporaryDirectory dir;
  asio::io_context io;
  Service s(io, config(dir.path));
  LocalDevelopmentIdentity identity;
  Api api(s, identity);
  EXPECT_EQ(api.handle("GET", "/api/v1/runs?limit=1&offset=0", "").status, 200U);
  EXPECT_EQ(api.handle("GET", "/api/v1/runs?limit=10000", "").status, 400U);
}
