#include "../support.hpp"
#include <fstream>
#include <iostream>
#include <laso/cli/cli.hpp>
#include <sstream>

using namespace laso;
using namespace laso::test;

namespace {
struct CliResult {
  int status;
  std::string output;
};

CliResult invoke_cli(std::vector<std::string> arguments) {
  std::vector<char *> argv;
  argv.reserve(arguments.size());
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  std::ostringstream output;
  auto *previous = std::cout.rdbuf(output.rdbuf());
  try {
    const auto status = cli_main(static_cast<int>(argv.size()), argv.data());
    std::cout.rdbuf(previous);
    return {status, output.str()};
  } catch (...) {
    std::cout.rdbuf(previous);
    throw;
  }
}
} // namespace

TEST(Cli, ScheduleAndTriggerDeleteReturnSuccess) {
  TemporaryDirectory directory;
  const auto pipeline = directory.path / "pipeline.yaml";
  const auto schedule = directory.path / "schedule.json";
  const auto trigger = directory.path / "trigger.json";
  std::ofstream(pipeline) << single();
  std::ofstream(schedule) << R"({
    "id": "cli-schedule",
    "name": "cli-schedule",
    "pipeline": "test@1",
    "type": "one_time",
    "at": "2099-01-01T00:00:00.000Z"
  })";
  std::ofstream(trigger) << R"({
    "id": "cli-trigger",
    "name": "cli-trigger",
    "pipeline": "test@1",
    "event": "cli.test"
  })";

  const auto data_dir = directory.path.string();
  ASSERT_EQ(invoke_cli({"laso", "--data-dir", data_dir, "pipeline", "register", pipeline.string()})
                .status,
            0);
  ASSERT_EQ(
      invoke_cli({"laso", "--data-dir", data_dir, "schedule", "create", schedule.string()}).status,
      0);
  const auto deleted_schedule =
      invoke_cli({"laso", "--data-dir", data_dir, "schedule", "delete", "cli-schedule"});
  ASSERT_EQ(deleted_schedule.status, 0);
  const Json expected_schedule{{"id", "cli-schedule"}, {"deleted", true}};
  EXPECT_EQ(Json::parse(deleted_schedule.output), expected_schedule);

  ASSERT_EQ(
      invoke_cli({"laso", "--data-dir", data_dir, "trigger", "create", trigger.string()}).status,
      0);
  const auto deleted_trigger =
      invoke_cli({"laso", "--data-dir", data_dir, "trigger", "delete", "cli-trigger"});
  ASSERT_EQ(deleted_trigger.status, 0);
  const Json expected_trigger{{"id", "cli-trigger"}, {"deleted", true}};
  EXPECT_EQ(Json::parse(deleted_trigger.output), expected_trigger);
}
