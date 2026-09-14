#include "../support.hpp"
#include <algorithm>
#include <fstream>
#include <laso/schema/validator.hpp>
#include <thread>
using namespace laso;
using namespace laso::test;
namespace {
void write_schema(const std::filesystem::path &root, const std::string &name, const Json &schema) {
  std::filesystem::create_directories(root);
  std::ofstream(root / name) << schema.dump(2);
}
std::string pipeline_with(const std::string &node) {
  return "laso: '1'\nname: schema-test\nversion: 1\nnodes:\n  action:\n    " + node + "\nedges:\n  - {from: input, to: action}\n  - {from: action, to: output}\n";
}
}
TEST(Schema, ValidAndInvalidPayloads) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  write_schema(root, "value.json", {{"type", "object"}, {"required", {"value"}}, {"properties", {{"value", {{"type", "integer"}}}}}});
  SchemaValidator validator({root});
  try { validator.validate("value.json", {{"value", 3}}, "n", "input"); }
  catch (const Error &error) { std::cerr << error.what() << " " << error.details.dump() << std::endl; FAIL(); }
  try {
    validator.validate("value.json", {{"value", "bad"}}, "n", "output");
    FAIL();
  } catch (const Error &error) {
    EXPECT_EQ(error.code, ErrorCode::Validation);
    EXPECT_EQ(error.details.at("direction"), "output");
    EXPECT_EQ(error.details.at("node"), "n");
  }
}
TEST(Schema, SecureLocalRefsAndRejectedRemoteRefs) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  write_schema(root, "common.json", {{"$defs", {{"id", {{"type", "integer"}}}}}});
  write_schema(root, "value.json", {{"type", "object"}, {"properties", {{"id", {{"$ref", "common.json#/$defs/id"}}}}}});
  SchemaValidator validator({root});
  EXPECT_NO_THROW(validator.validate_declaration("value.json"));
  write_schema(root, "remote.json", {{"$ref", "https://example.invalid/schema.json"}});
  EXPECT_THROW(validator.validate_declaration("remote.json"), Error);
  EXPECT_THROW(validator.validate_declaration("../schemas/value.json"), Error);
  EXPECT_THROW(validator.validate_declaration("/etc/passwd"), Error);
}
TEST(Schema, RuntimeOutputContractFailure) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  write_schema(root, "integer.json", {{"type", "integer"}});
  asio::io_context io;
  auto c = config(dir.path);
  c.schema_roots = {root};
  Service service(io, c);
  service.functions().add("bad", std::make_shared<Function>([](ExecutionContext &, const Json &) -> Task<Json> { co_return "not an integer"; }));
  auto run = execute(service, io, pipeline_with("type: function\n    function: bad\n    output_schema: integer.json"), 1);
  EXPECT_EQ(run.state, RunState::Failed);
  EXPECT_EQ(service.list(RecordKind::Attempt, run.id).back().at("state"), "Failed");
}
TEST(Schema, RuntimeInputContractPreventsInvocation) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  write_schema(root, "integer.json", {{"type", "integer"}});
  asio::io_context io;
  auto c = config(dir.path);
  c.schema_roots = {root};
  Service service(io, c);
  bool invoked = false;
  service.functions().add("observe", std::make_shared<Function>([&](ExecutionContext &, const Json &input) -> Task<Json> {
    invoked = true;
    co_return input;
  }));
  auto run = execute(service, io, pipeline_with("type: function\n    function: observe\n    input_schema: integer.json"), Json{{"wrong", true}});
  EXPECT_EQ(run.state, RunState::Failed);
  EXPECT_FALSE(invoked);
  EXPECT_NE(service.list(RecordKind::Attempt, run.id).back().at("error").get<std::string>().find("schema=integer.json"), std::string::npos);
}
TEST(Schema, PayloadLimitIsEnforced) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  write_schema(root, "any.json", {{}});
  SchemaLimits limits;
  limits.max_payload_bytes = 8;
  SchemaValidator validator({root}, limits);
  EXPECT_THROW(validator.validate("any.json", std::string(100, 'x'), "n", "input"), Error);
}
TEST(Schema, ConcurrentValidationUsesThreadSafeCache) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  write_schema(root, "integer.json", {{"type", "integer"}});
  SchemaValidator validator({root});
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 8; ++i)
    workers.emplace_back([&] {
      try { validator.validate("integer.json", 7, "n", "input"); }
      catch (...) { ++failures; }
    });
  for (auto &worker : workers) worker.join();
  EXPECT_EQ(failures.load(), 0U);
}
TEST(Schema, ValidatorNodeUsesSchemaEngine) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  write_schema(root, "object.json", {{"type", "object"}, {"required", {"ok"}}});
  asio::io_context io;
  auto c = config(dir.path);
  c.schema_roots = {root};
  Service service(io, c);
  auto yaml = "laso: '1'\nname: validator-schema\nversion: 1\nnodes:\n  verify:\n    type: validator\n    schema: object.json\n  accepted:\n    type: function\n    function: identity\n  rejected:\n    type: function\n    function: identity\nedges:\n  - {from: input, to: verify}\n  - {from: verify, to: accepted, condition: accepted}\n  - {from: verify, to: rejected, condition: rejected}\n  - {from: accepted, to: output}\n  - {from: rejected, to: output}\n";
  auto run = execute(service, io, yaml, {{"ok", true}});
  EXPECT_EQ(run.state, RunState::Completed);
  EXPECT_TRUE(std::any_of(run.message.provenance.begin(), run.message.provenance.end(),
                          [](const auto &record) { return record.validation == "validated"; }));
}
TEST(Schema, MissingAndMalformedSchemasFailRegistration) {
  TemporaryDirectory dir;
  auto root = dir.path / "schemas";
  std::filesystem::create_directories(root);
  std::ofstream(root / "bad.json") << "{not-json";
  write_schema(root, "invalid.json", {{"type", "not-a-json-type"}});
  asio::io_context io;
  auto c = config(dir.path);
  c.schema_roots = {root};
  Service service(io, c);
  EXPECT_THROW(service.register_pipeline(pipeline_with("type: function\n    function: identity\n    output_schema: missing.json")), Error);
  EXPECT_THROW(service.register_pipeline(pipeline_with("type: function\n    function: identity\n    output_schema: bad.json")), Error);
  EXPECT_THROW(service.register_pipeline(pipeline_with("type: function\n    function: identity\n    output_schema: invalid.json")), Error);
}
