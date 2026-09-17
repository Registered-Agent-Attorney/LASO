#include "../support.hpp"
#include <cstdlib>
#include <fstream>
#include <laso/workers/process_transport.hpp>
#include <sys/types.h>
#include <unistd.h>

using namespace laso;
using namespace laso::test;

namespace {
ProcessWorkerConfig opencode_config(const std::filesystem::path &root, unsigned port) {
  ProcessWorkerConfig result;
  result.executable = LASO_OPENCODE_WORKER;
  result.args = {"--opencode", std::getenv("OPENCODE_BIN") ? std::getenv("OPENCODE_BIN") : "opencode",
                 "--port", std::to_string(port), "--allowed-root", root.string(), "--timeout-ms", "120000"};
  result.startup_timeout_ms = 120000;
  result.request_timeout_ms = 120000;
  return result;
}

WorkerRequest opencode_request(const std::filesystem::path &root, const std::string &key,
                               const std::string &instructions, const std::string &session = {}) {
  WorkerRequest request;
  request.job_id = key;
  request.worker_id = "opencode";
  request.task_type = "coding";
  request.instructions = instructions;
  request.idempotency_key = key;
  request.run_id = "opencode-run";
  request.node_id = "coding";
  request.input = Json::object();
  request.metadata = {{"project_dir", root.string()}};
  if (!session.empty())
    request.metadata["opencode_session_id"] = session;
  return request;
}
} // namespace

TEST(OpenCodeWorker, RealInstalledAdapterCreatesAndContinuesSession) {
  if (!std::getenv("LASO_RUN_REAL_OPENCODE"))
    GTEST_SKIP() << "Set LASO_RUN_REAL_OPENCODE=1 to run the configured OpenCode integration";
  TemporaryDirectory root;
  std::ofstream(root.path / "fixture.txt") << "before\n";
  const auto port = 19000U + static_cast<unsigned>(getpid() % 1000);
  ProcessWorkerTransport transport("opencode", opencode_config(root.path, port));
  ASSERT_NO_THROW(transport.start());
  ASSERT_TRUE(transport.metadata().healthy);
  auto first = transport.submit(opencode_request(
      root.path, "opencode-turn-1",
      "Read fixture.txt, replace its contents with exactly 'after', and reply with DONE. "
      "Do not modify any other file. You may use local tools in this project."));
  ASSERT_EQ(first.state, WorkerJobState::Completed) << first.error;
  ASSERT_TRUE(first.result.is_object());
  const auto session = first.result.value("session_id", std::string{});
  ASSERT_FALSE(session.empty());
  ASSERT_TRUE(std::filesystem::exists(root.path / "fixture.txt"));
  std::ifstream changed(root.path / "fixture.txt");
  std::string contents;
  std::getline(changed, contents);
  EXPECT_EQ(contents, "after");
  const auto second = transport.submit(opencode_request(
      root.path, "opencode-turn-2", "Reply with CONTINUED and do not change any files.", session));
  EXPECT_EQ(second.state, WorkerJobState::Completed) << second.error;
  EXPECT_EQ(second.result.value("session_id", std::string{}), session);
  transport.stop();

  ProcessWorkerTransport restarted("opencode", opencode_config(root.path, port));
  ASSERT_NO_THROW(restarted.start());
  const auto after_restart = restarted.submit(opencode_request(
      root.path, "opencode-turn-3", "Reply with RECOVERED and do not change any files.", session));
  EXPECT_EQ(after_restart.state, WorkerJobState::Completed) << after_restart.error;
  EXPECT_EQ(after_restart.result.value("session_id", std::string{}), session);
  restarted.stop();
}

TEST(OpenCodeWorker, RejectsProjectOutsideConfiguredRoot) {
  TemporaryDirectory root;
  TemporaryDirectory outside;
  const auto port = 20000U + static_cast<unsigned>(getpid() % 1000);
  ProcessWorkerTransport transport("opencode", opencode_config(root.path, port));
  if (std::getenv("LASO_RUN_REAL_OPENCODE"))
    ASSERT_NO_THROW(transport.start());
  else
    GTEST_SKIP() << "The adapter startup requires the configured OpenCode binary";
  auto request = opencode_request(outside.path, "opencode-outside", "Do nothing.");
  const auto result = transport.submit(request);
  EXPECT_EQ(result.state, WorkerJobState::Failed);
  EXPECT_NE(result.error.find("outside an allowed root"), std::string::npos);
}
