#include "../support.hpp"
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

TEST(DistributedExecution, ParallelBranchesUseDurableNodeWork) {
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
  first.max_runs = 2;
  first.max_nodes = 4;
  first.max_nodes_per_run = 2;
  first.coordination_lease_ttl_ms = 1000;
  first.coordination_heartbeat_interval_ms = 100;
  first.validate();
  auto second = first;
  second.data_dir = second_data.path;
  second.db_path.clear();

  Executor first_executor(first.workers), second_executor(second.workers);
  Service first_service(first_executor.context(), first);
  Service second_service(second_executor.context(), second);
  const auto pipeline = R"yaml(
laso: '1'
name: distributed-parallel
version: 1
nodes:
  input:
    type: input
  fork:
    type: parallel
    join: join
  left:
    type: function
    function: identity
  right:
    type: function
    function: identity
  join:
    type: join
  output:
    type: output
edges:
  - {from: input, to: fork}
  - {from: fork, to: left}
  - {from: fork, to: right}
  - {from: left, to: join}
  - {from: right, to: join}
  - {from: join, to: output}
)yaml";
  first_service.register_pipeline(pipeline);
  first_executor.start();
  second_executor.start();
  const auto run_id = first_service.start("distributed-parallel@1", Json{{"value", "shared"}});
  laso::Run result;
  for (unsigned attempt = 0; attempt < 400; ++attempt) {
    result = first_service.get(RecordKind::Run, run_id).get<laso::Run>();
    if (terminal(result.state))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto work = first_service.list(RecordKind::NodeWork, run_id);
  const auto attempts = first_service.list(RecordKind::Attempt, run_id);
  first_service.shutdown();
  second_service.shutdown();
  first_executor.join();
  second_executor.join();
  std::string work_error;
  for (const auto &item : work)
    work_error += item.dump() + "\n";
  ASSERT_EQ(result.state, RunState::Completed) << result.error << " pending="
                                                << result.pending_parallel_group << " work="
                                                << work_error;
  ASSERT_EQ(work.size(), 2U);
  EXPECT_EQ(work[0].get<NodeWork>().state, NodeWorkState::Completed);
  EXPECT_EQ(work[1].get<NodeWork>().state, NodeWorkState::Completed);
  bool fork_completed = false;
  for (const auto &value : attempts)
    if (value.get<NodeExecution>().node_id == "fork" &&
        value.get<NodeExecution>().state == NodeState::Completed)
      fork_completed = true;
  EXPECT_TRUE(fork_completed);
  EXPECT_EQ(result.message.payload, Json::array({Json{{"value", "shared"}},
                                                 Json{{"value", "shared"}}}));
}

TEST(DistributedExecution, ProcessCrashAllowsNodeWorkTakeoverAndRunRecovery) {
  IsolatedSchema database;
  if (database.dsn.empty())
    GTEST_SKIP() << "LASO_TEST_POSTGRES_DSN is not configured";
#if !defined(LASO_DISTRIBUTED_PROCESS)
  GTEST_SKIP() << "distributed process fixture is not built";
#else
  TemporaryDirectory directory;
  Config configuration = config(directory.path);
  configuration.storage_backend = "postgres";
  configuration.postgres_dsn = database.dsn;
  configuration.postgres_schema = database.schema;
  configuration.execution_mode = "multi_instance";
  configuration.max_runs = 1;
  configuration.max_nodes = 1;
  configuration.max_nodes_per_run = 1;
  configuration.coordination_lease_ttl_ms = 1000;
  configuration.coordination_heartbeat_interval_ms = 100;
  configuration.validate();

  asio::io_context io;
  Service seed(io, configuration);
  const auto pipeline = R"yaml(
laso: '1'
name: process-recovery
version: 1
nodes:
  input: {type: input}
  fork: {type: parallel, join: join}
  left: {type: function, function: distributed_hold}
  right: {type: function, function: identity}
  join: {type: join}
  output: {type: output}
edges:
  - {from: input, to: fork}
  - {from: fork, to: left}
  - {from: fork, to: right}
  - {from: left, to: join}
  - {from: right, to: join}
  - {from: join, to: output}
)yaml";
  seed.register_pipeline(pipeline);
  const auto run_id = seed.start("process-recovery@1", Json{{"value", "crash-safe"}});
  seed.shutdown();

  const auto start_process = [&](const char *hold_ms) {
    const auto child = fork();
    if (child == -1) {
      ADD_FAILURE() << "fork failed";
      return pid_t{-1};
    }
    if (child == 0) {
      setenv("LASO_DISTRIBUTED_TEST_DSN", database.dsn.c_str(), 1);
      setenv("LASO_DISTRIBUTED_TEST_SCHEMA", database.schema.c_str(), 1);
      setenv("LASO_DISTRIBUTED_TEST_RUN_ID", run_id.c_str(), 1);
      setenv("LASO_DISTRIBUTED_TEST_HOLD_MS", hold_ms, 1);
      execl(LASO_DISTRIBUTED_PROCESS, LASO_DISTRIBUTED_PROCESS, nullptr);
      _exit(127);
    }
    return child;
  };
  const auto owner = start_process("10000");
  ASSERT_NE(owner, -1);
  std::string old_owner;
  std::uint64_t old_token = 0;
  std::string old_attempt_id;
  std::string work_id;
  for (unsigned i = 0; i < 300; ++i) {
    for (const auto &value : seed.list(RecordKind::NodeWork, run_id)) {
      const auto work = value.get<NodeWork>();
      if (work.state == NodeWorkState::Running) {
        work_id = work.id;
        old_owner = work.owner_instance_id;
        old_token = work.fencing_token;
        old_attempt_id = work.attempt_id;
      }
    }
    if (!work_id.empty())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  ASSERT_FALSE(work_id.empty());
  ASSERT_EQ(kill(owner, SIGKILL), 0);
  int owner_status = 0;
  ASSERT_EQ(waitpid(owner, &owner_status, 0), owner);
  ASSERT_TRUE(WIFSIGNALED(owner_status));
  const auto recovery = start_process("0");
  laso::Run result;
  for (unsigned i = 0; i < 600; ++i) {
    result = seed.get(RecordKind::Run, run_id).get<laso::Run>();
    if (terminal(result.state))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  int recovery_status = 0;
  ASSERT_EQ(waitpid(recovery, &recovery_status, 0), recovery);
  EXPECT_TRUE(WIFEXITED(recovery_status));
  ASSERT_EQ(result.state, RunState::Completed) << result.error;
  const auto works = seed.list(RecordKind::NodeWork, run_id);
  ASSERT_EQ(works.size(), 2U);
  bool retaken = false;
  for (const auto &value : works) {
    const auto work = value.get<NodeWork>();
    EXPECT_EQ(work.state, NodeWorkState::Completed);
    if (work.id == work_id) {
      retaken = work.attempt >= 2 && work.fencing_token > old_token &&
                work.owner_instance_id != old_owner && !work.attempt_id.empty() &&
                work.attempt_id != old_attempt_id;
    }
  }
  EXPECT_TRUE(retaken);
#endif
}

TEST(DistributedExecution, StaleNodeCompletionIsRejectedByFencing) {
  IsolatedSchema database;
  if (database.dsn.empty())
    GTEST_SKIP() << "LASO_TEST_POSTGRES_DSN is not configured";
#if defined(LASO_HAS_POSTGRES)
  StorageOptions storage_options;
  storage_options.backend = "postgres";
  storage_options.postgres_dsn = database.dsn;
  storage_options.postgres_schema = database.schema;
  storage_options.allow_multiple_processes = true;
  auto storage = create_storage(storage_options);
  CoordinationOptions coordination_options;
  coordination_options.postgres_dsn = database.dsn;
  coordination_options.postgres_schema = database.schema;
  auto first = create_coordination(coordination_options, "stale-first");
  auto second = create_coordination(coordination_options, "stale-second");
  NodeWork work;
  work.id = "stale-node-work";
  work.run_id = "run-1";
  work.group_id = "group-1";
  work.node_id = "branch";
  work.join = "join";
  work.token = {"branch", Message{}, {{"group-1", "join", 1, 0}}};
  storage->commit({{RecordKind::NodeWork, work.id, work.run_id, Json(work)}});
  const auto old = first->acquire("node:" + work.id, 100);
  ASSERT_TRUE(old);
  work.state = NodeWorkState::Running;
  work.attempt = 1;
  storage->commit_owned({{RecordKind::NodeWork, work.id, work.run_id, Json(work)}},
                        "node:" + work.id, old->owner_instance, old->fencing_token);
  std::this_thread::sleep_for(std::chrono::milliseconds{180});
  const auto current = second->acquire("node:" + work.id, 5000);
  ASSERT_TRUE(current);
  EXPECT_GT(current->fencing_token, old->fencing_token);
  work.state = NodeWorkState::Completed;
  work.result = Message{};
  EXPECT_THROW(storage->commit_owned({{RecordKind::NodeWork, work.id, work.run_id, Json(work)}},
                                     "node:" + work.id, old->owner_instance,
                                     old->fencing_token),
               Error);
  storage->commit_owned({{RecordKind::NodeWork, work.id, work.run_id, Json(work)}},
                        "node:" + work.id, current->owner_instance,
                        current->fencing_token);
  EXPECT_EQ(storage->get(RecordKind::NodeWork, work.id).get<NodeWork>().state,
            NodeWorkState::Completed);
#else
  GTEST_SKIP() << "PostgreSQL backend is not enabled";
#endif
}

TEST(DistributedExecution, CancellationPropagatesToRemoteNodeWork) {
  IsolatedSchema database;
  if (database.dsn.empty())
    GTEST_SKIP() << "LASO_TEST_POSTGRES_DSN is not configured";
#if defined(LASO_DISTRIBUTED_PROCESS)
  TemporaryDirectory directory;
  Config configuration = config(directory.path);
  configuration.storage_backend = "postgres";
  configuration.postgres_dsn = database.dsn;
  configuration.postgres_schema = database.schema;
  configuration.execution_mode = "multi_instance";
  configuration.max_runs = 1;
  configuration.max_nodes = 1;
  configuration.max_nodes_per_run = 1;
  configuration.coordination_lease_ttl_ms = 1000;
  configuration.coordination_heartbeat_interval_ms = 100;
  configuration.validate();
  asio::io_context io;
  Service controller(io, configuration);
  const auto pipeline = R"yaml(
laso: '1'
name: process-cancel
version: 1
nodes:
  input: {type: input}
  fork: {type: parallel, join: join}
  left: {type: function, function: distributed_hold}
  right: {type: function, function: identity}
  join: {type: join}
  output: {type: output}
edges:
  - {from: input, to: fork}
  - {from: fork, to: left}
  - {from: fork, to: right}
  - {from: left, to: join}
  - {from: right, to: join}
  - {from: join, to: output}
)yaml";
  controller.register_pipeline(pipeline);
  const auto run_id = controller.start("process-cancel@1");
  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    setenv("LASO_DISTRIBUTED_TEST_DSN", database.dsn.c_str(), 1);
    setenv("LASO_DISTRIBUTED_TEST_SCHEMA", database.schema.c_str(), 1);
    setenv("LASO_DISTRIBUTED_TEST_RUN_ID", run_id.c_str(), 1);
    setenv("LASO_DISTRIBUTED_TEST_HOLD_MS", "10000", 1);
    execl(LASO_DISTRIBUTED_PROCESS, LASO_DISTRIBUTED_PROCESS, nullptr);
    _exit(127);
  }
  bool running = false;
  for (unsigned i = 0; i < 300; ++i) {
    for (const auto &value : controller.list(RecordKind::NodeWork, run_id))
      running = running || value.get<NodeWork>().state == NodeWorkState::Running;
    if (running)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  ASSERT_TRUE(running);
  controller.runtime().cancel(run_id);
  auto result = controller.get(RecordKind::Run, run_id).get<laso::Run>();
  for (unsigned i = 0; i < 300 && !terminal(result.state); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    result = controller.get(RecordKind::Run, run_id).get<laso::Run>();
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(result.state, RunState::Cancelled);
  for (const auto &value : controller.list(RecordKind::NodeWork, run_id))
    EXPECT_EQ(value.get<NodeWork>().state, NodeWorkState::Cancelled);
#else
  GTEST_SKIP() << "distributed process fixture is not built";
#endif
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
