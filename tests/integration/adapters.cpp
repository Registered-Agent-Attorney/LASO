#include "../support.hpp"
#include <boost/beast.hpp>
#include <fstream>
#include <laso/api/api.hpp>

using namespace laso;
using namespace laso::test;
TEST(Storage, PersistsAcrossConnections) {
  TemporaryDirectory dir;
  {
    SQLiteStorage s(dir.path / "state.db");
    s.commit({{RecordKind::Pipeline, "example", "", {{"value", 42}}}});
  }
  {
    SQLiteStorage s(dir.path / "state.db");
    EXPECT_EQ(s.get(RecordKind::Pipeline, "example").at("value"), 42);
  }
}
TEST(Storage, TransactionRollsBackWholeCheckpoint) {
  TemporaryDirectory dir;
  SQLiteStorage s(dir.path / "state.db");
  std::vector<Record> batch{
      {RecordKind::Run, "first", "first", {{"valid", true}}},
      {RecordKind::Message, "large", "first", std::string(4 * 1024 * 1024 + 1, 'a')}};
  EXPECT_THROW(s.commit(batch), Error);
  EXPECT_THROW(s.get(RecordKind::Run, "first"), Error);
}
TEST(Storage, ProcessLeasePreventsCompetingExecutors) {
  TemporaryDirectory dir;
  ProcessLease first(dir.path / "state.db");
  EXPECT_THROW(ProcessLease(dir.path / "state.db"), Error);
}
TEST(Artifacts, IgnoresUntrustedNamesForPath) {
  TemporaryDirectory dir;
  SQLiteStorage storage(dir.path / "state.db");
  LocalArtifactStore artifacts(dir.path / "artifacts", storage);
  Artifact a;
  a.name = "../../outside";
  a.run_id = "../../outside";
  std::string data = "example";
  auto saved = artifacts.put(a, std::as_bytes(std::span(data.data(), data.size())));
  EXPECT_EQ(std::filesystem::path(saved.location).parent_path(), dir.path / "artifacts");
  EXPECT_TRUE(std::filesystem::exists(saved.location));
  EXPECT_EQ(storage.get(RecordKind::Artifact, saved.id).at("name"), "../../outside");
}
TEST(Plugins, DiscoversLoadsInvokesAndUnloadsExample) {
  TemporaryDirectory dir;
  asio::io_context io;
  auto c = config(dir.path);
  c.plugin_dirs = {LASO_PLUGIN_DIR};
  Service s(io, c);
  ASSERT_EQ(s.plugins().size(), 2U);
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
    SQLiteStorage s(c.db_path);
    s.commit({{RecordKind::Run, r.id, r.id, Json(r)}});
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
    SQLiteStorage s(c.db_path);
    s.commit({{RecordKind::Run, r.id, r.id, Json(r)}});
  }
  asio::io_context io;
  Service s(io, c);
  EXPECT_EQ(s.get(RecordKind::Run, r.id).get<laso::Run>().state, RunState::Cancelled);
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
