#include "../support.hpp"
#include <chrono>
#include <thread>

using namespace laso;
using namespace laso::test;

#if defined(LASO_HAS_POSTGRES)
namespace {
std::string test_dsn() {
  const auto *value = std::getenv("LASO_TEST_POSTGRES_DSN");
  return value && *value ? value : std::string{};
}

struct IsolatedSchema {
  std::string dsn = test_dsn();
  std::string schema = "distributed_" + uuid();
  IsolatedSchema() {
    std::replace(schema.begin(), schema.end(), '-', '_');
  }
  ~IsolatedSchema() {
    if (dsn.empty())
      return;
    try {
      pqxx::connection connection(dsn);
      pqxx::work transaction(connection);
      transaction.exec("DROP SCHEMA IF EXISTS \"" + schema + "\" CASCADE");
      transaction.commit();
    } catch (...) {
    }
  }
};
} // namespace

TEST(DistributedExecution, TwoServicesSharePostgresAndOneCompletesQueuedRun) {
  IsolatedSchema database;
  if (database.dsn.empty())
    GTEST_SKIP() << "LASO_TEST_POSTGRES_DSN is not configured";
  TemporaryDirectory first_data;
  TemporaryDirectory second_data;
  Config first = config(first_data.path);
  first.storage_backend = "postgres";
  first.postgres_dsn = database.dsn;
  first.postgres_schema = database.schema;
  first.execution_mode = "multi_instance";
  first.max_runs = 1;
  first.validate();
  auto second = first;
  second.data_dir = second_data.path;
  second.db_path.clear();

  Executor first_executor(first.workers), second_executor(second.workers);
  Service first_service(first_executor.context(), first);
  Service second_service(second_executor.context(), second);
  const auto pipeline = R"yaml(
laso: '1'
name: distributed
version: 1
nodes:
  input:
    type: input
  transform:
    type: function
    function: identity
  output:
    type: output
edges:
  - from: input
    to: transform
  - from: transform
    to: output
)yaml";
  first_service.register_pipeline(pipeline);
  first_executor.start();
  second_executor.start();
  const auto run_id = first_service.start("distributed@1", Json{{"value", "shared"}});
  laso::Run result;
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    result = first_service.get(RecordKind::Run, run_id).get<laso::Run>();
    if (terminal(result.state))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  first_service.shutdown();
  second_service.shutdown();
  first_executor.join();
  second_executor.join();
  EXPECT_EQ(result.state, RunState::Completed);
  EXPECT_FALSE(result.owner_instance_id.empty());
  EXPECT_EQ(result.pipeline_version, 1U);
  EXPECT_EQ(result.message.payload, Json({{"value", "shared"}}));
}

TEST(DistributedExecution, SubpipelineReleasesParentOwnershipWhileChildRuns) {
  IsolatedSchema database;
  if (database.dsn.empty())
    GTEST_SKIP() << "LASO_TEST_POSTGRES_DSN is not configured";
  TemporaryDirectory first_data;
  TemporaryDirectory second_data;
  Config first = config(first_data.path);
  first.storage_backend = "postgres";
  first.postgres_dsn = database.dsn;
  first.postgres_schema = database.schema;
  first.execution_mode = "multi_instance";
  first.max_runs = 1;
  first.validate();
  auto second = first;
  second.data_dir = second_data.path;
  second.db_path.clear();

  Executor first_executor(first.workers), second_executor(second.workers);
  Service first_service(first_executor.context(), first);
  Service second_service(second_executor.context(), second);
  const auto child_pipeline = R"yaml(
laso: '1'
name: child
version: 1
nodes:
  input:
    type: input
  transform:
    type: function
    function: identity
  output:
    type: output
edges:
  - from: input
    to: transform
  - from: transform
    to: output
)yaml";
  const auto parent_pipeline = R"yaml(
laso: '1'
name: parent
version: 1
nodes:
  input:
    type: input
  child:
    type: subpipeline
    pipeline: child@1
  output:
    type: output
edges:
  - from: input
    to: child
  - from: child
    to: output
)yaml";
  first_service.register_pipeline(child_pipeline);
  first_service.register_pipeline(parent_pipeline);
  first_executor.start();
  second_executor.start();
  const auto run_id = first_service.start("parent@1", Json{{"value", "nested"}});
  laso::Run result;
  for (unsigned attempt = 0; attempt < 400; ++attempt) {
    result = first_service.get(RecordKind::Run, run_id).get<laso::Run>();
    if (terminal(result.state))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  first_service.shutdown();
  second_service.shutdown();
  first_executor.join();
  second_executor.join();
  ASSERT_EQ(result.state, RunState::Completed);
  ASSERT_EQ(result.child_runs.size(), 1U);
  const auto child = first_service.get(RecordKind::Run, result.child_runs.front()).get<laso::Run>();
  EXPECT_EQ(child.state, RunState::Completed);
  EXPECT_EQ(result.message.payload, Json({{"value", "nested"}}));
}

TEST(DistributedExecution, MultiInstanceRejectsSQLite) {
  TemporaryDirectory directory;
  auto configuration = config(directory.path);
  configuration.execution_mode = "multi_instance";
  EXPECT_THROW(configuration.validate(), Error);
}
#else
TEST(DistributedExecution, PostgreSQLBackendNotBuilt) {
  GTEST_SKIP() << "LASO_ENABLE_POSTGRES is not enabled";
}
#endif
