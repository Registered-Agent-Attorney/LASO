#include "../support.hpp"
#include <laso/policies/policy.hpp>
#include <laso_plugin.h>

using namespace laso;
using namespace laso::test;
TEST(Pipeline, ParsesTypedDefinition) {
  auto p = parse_pipeline(fixture("hello-pipeline"));
  EXPECT_EQ(p.name, "hello");
  EXPECT_EQ(p.schema_version, 1U);
  EXPECT_EQ(p.nodes.at("greet").binding, "hello");
  EXPECT_EQ(p.nodes.size(), 3U);
}
TEST(Pipeline, RejectsUnsupportedVersion) {
  auto yaml = single();
  yaml.replace(yaml.find("'1'"), 3, "'2'");
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RequiresVersion) {
  auto yaml = single();
  yaml.erase(yaml.find("version: 1\n"), 11);
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RejectsDuplicateNode) {
  EXPECT_THROW(parse_pipeline("laso: '1'\nname: bad\nversion: 1\nnodes:\n  x: {type: input}\n  x: "
                              "{type: output}\nedges: []"),
               Error);
}
TEST(Pipeline, RejectsDuplicateRootField) {
  EXPECT_THROW(parse_pipeline(single() + "name: duplicate\n"), Error);
}
TEST(Pipeline, RejectsMissingTarget) {
  auto yaml = single();
  yaml.replace(yaml.find("to: output"), 10, "to: absent");
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RejectsMalformedEdge) {
  auto yaml = single();
  yaml.replace(yaml.find("to: output"), 10, "target: output");
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RejectsMalformedEdgeIdentifiers) {
  auto yaml = single();
  yaml.replace(yaml.find("to: output"), 10, "to: ../output");
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RejectsUnknownNode) {
  EXPECT_THROW(parse_pipeline(single("type: unknown")), Error);
}
TEST(Pipeline, RejectsWrongBindingField) {
  EXPECT_THROW(parse_pipeline(single("type: tool\n    function: echo")), Error);
}
TEST(Pipeline, RejectsUnboundedCycle) {
  auto yaml = fixture("bounded-loop");
  auto position = yaml.find(", max_iterations: 2}");
  yaml.erase(position, 19);
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RequiresLoopLimit) {
  auto yaml = fixture("bounded-loop");
  auto pos = yaml.find("type: loop, max_iterations: 2");
  yaml.replace(pos, std::string("type: loop, max_iterations: 2").size(), "type: loop");
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RejectsUnknownCondition) {
  auto yaml = fixture("bounded-loop");
  auto pos = yaml.find("condition: repeat");
  yaml.replace(pos, 17, "condition: arbitrary");
  EXPECT_THROW(parse_pipeline(yaml), Error);
}
TEST(Pipeline, RejectsUnsafeTag) {
  EXPECT_THROW(parse_pipeline("!!python/object/apply:os.system ['echo unsafe']"), Error);
}
TEST(Pipeline, RejectsAliasRecursion) {
  EXPECT_THROW(parse_pipeline("a: &a [*a]"), Error);
}
TEST(Pipeline, RejectsOversizedInput) {
  EXPECT_THROW(parse_pipeline(std::string(max_document_bytes + 1, 'x')), Error);
}
TEST(Pipeline, RejectsMultipleDocuments) {
  EXPECT_THROW(parse_pipeline(single() + "---\na: b"), Error);
}
TEST(Pipeline, SupportsCustomNodeContracts) {
  EXPECT_NO_THROW(parse_pipeline(single("type: custom"), {"custom"}));
}
TEST(State, ValidTransitionsAndTerminalImmutability) {
  EXPECT_TRUE(valid_transition(RunState::Queued, RunState::Starting));
  EXPECT_TRUE(valid_transition(RunState::Running, RunState::WaitingApproval));
  EXPECT_TRUE(valid_transition(RunState::WaitingApproval, RunState::Queued));
  EXPECT_FALSE(valid_transition(RunState::Completed, RunState::Running));
  EXPECT_FALSE(valid_transition(RunState::Queued, RunState::Completed));
  EXPECT_FALSE(valid_transition(RunState::Queued, RunState::Paused));
  EXPECT_FALSE(valid_transition(RunState::WaitingTool, RunState::WaitingApproval));
  EXPECT_TRUE(valid_transition(RunState::Running, RunState::Cancelled));
}
TEST(Message, RoundTripsEnvelope) {
  Message m;
  m.payload = {{"value", 1}};
  m.confidence = 0.8;
  auto copy = Json(m).get<Message>();
  EXPECT_EQ(copy.id, m.id);
  EXPECT_EQ(copy.payload, m.payload);
  EXPECT_EQ(copy.confidence, m.confidence);
}
TEST(Message, RejectsInvalidConfidence) {
  Message m;
  Json json = m;
  json["confidence"] = 2.0;
  EXPECT_THROW(json.get<Message>(), Error);
}
TEST(Policy, AllowsLocal) {
  PolicyEngine p;
  EXPECT_EQ(p.evaluate({}).decision, PolicyDecision::Allow);
}
TEST(Policy, DeniesNetworkByDefault) {
  PolicyEngine p;
  PolicyContext context;
  context.network = true;
  EXPECT_EQ(p.evaluate(context).decision, PolicyDecision::Deny);
}
TEST(Policy, ExplicitDenyWins) {
  PolicyEngine p({{"echo", PolicyDecision::Deny}});
  PolicyContext context;
  context.resource = "echo";
  EXPECT_EQ(p.evaluate(context).decision, PolicyDecision::Deny);
}
TEST(Policy, ApprovalRequirement) {
  PolicyEngine p;
  PolicyContext context;
  context.approval_required = true;
  EXPECT_EQ(p.evaluate(context).decision, PolicyDecision::RequireApproval);
}
TEST(Policy, RemotePrivateDataDenied) {
  PolicyEngine p({}, true);
  PolicyContext context;
  context.classification = "restricted";
  context.remote = true;
  EXPECT_EQ(p.evaluate(context).decision, PolicyDecision::Deny);
}
TEST(Registry, ToolNamesAndDuplicateProtection) {
  ToolRegistry r;
  r.add("echo", std::make_shared<EchoTool>());
  EXPECT_EQ(r.names().size(), 1U);
  EXPECT_THROW(r.add("echo", std::make_shared<EchoTool>()), Error);
  EXPECT_THROW(r.get("missing"), Error);
}
TEST(Registry, ProviderLookup) {
  ProviderRegistry r;
  r.add("mock", std::make_shared<MockModelProvider>());
  EXPECT_TRUE(r.get("mock")->health().healthy);
}
TEST(Registry, BatchIsAtomic) {
  ToolRegistry r;
  r.add("echo", std::make_shared<EchoTool>());
  EXPECT_THROW(
      r.add_batch({{"a", std::make_shared<EchoTool>()}, {"echo", std::make_shared<EchoTool>()}}),
      Error);
  EXPECT_EQ(r.names().size(), 1U);
}
TEST(Provider, MockIsOfflineAndStructured) {
  asio::io_context io;
  MockModelProvider p;
  ExecutionContext c{"r", "p", "n", {}, std::chrono::steady_clock::now() + Milliseconds{1000}};
  ModelRequest request{"mock", "", {{"question", "example"}}, Json::object()};
  auto future = asio::co_spawn(io, p.generate(request, c), asio::use_future);
  io.run();
  EXPECT_EQ(future.get().output.at("reviewed"), true);
  EXPECT_FALSE(p.metadata().remote);
}
TEST(Provider, LocalOpenAIAdapterRequiresLoopbackHTTP) {
  LocalOpenAICompatibleProvider provider("http://127.0.0.1:18081");
  EXPECT_TRUE(provider.metadata().network);
  EXPECT_FALSE(provider.metadata().remote);
  EXPECT_THROW(LocalOpenAICompatibleProvider("https://127.0.0.1:18081"), Error);
  EXPECT_THROW(LocalOpenAICompatibleProvider("http://example.invalid:18081"), Error);
}
TEST(Configuration, RejectsRemoteAnonymousDefault) {
  Config c;
  c.api_host = "0.0.0.0";
  EXPECT_THROW(c.validate(), Error);
  c.allow_remote_api = true;
  EXPECT_NO_THROW(c.validate());
}
TEST(Configuration, RejectsZeroConcurrency) {
  Config c;
  c.max_runs = 0;
  EXPECT_THROW(c.validate(), Error);
}
TEST(Configuration, ValidatesPerRunNodeLimit) {
  Config c;
  c.max_nodes_per_run = 0;
  EXPECT_THROW(c.validate(), Error);
  c.max_nodes_per_run = c.max_nodes + 1;
  EXPECT_THROW(c.validate(), Error);
}
TEST(PluginABI, IsPlainCVersionedBoundary) {
  EXPECT_EQ(LASO_PLUGIN_ABI_VERSION, 1U);
  EXPECT_GE(sizeof(laso_host_api), sizeof(void *) * 2);
}
