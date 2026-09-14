#include <laso/nodes/node.hpp>

namespace laso {
Task<NodeResult> FunctionNode::execute(ExecutionContext &c, const Message &input) {
  c.check();
  auto message = input;
  message.payload = co_await (*function_)(c, input.payload);
  c.check();
  co_return NodeResult{message, {}};
}
Task<NodeResult> AgentNode::execute(ExecutionContext &c, const Message &input) {
  auto permit = co_await limiter_.acquire(c);
  ModelRequest request{binding_.model, prompt_, input.payload, binding_.options};
  auto response = co_await provider_->generate(request, c);
  c.check();
  auto message = input;
  message.payload = std::move(response.output);
  message.provenance.push_back(
      {c.node_id, "", response.model, response.provider, "", input.id, "untrusted", timestamp()});
  co_return NodeResult{message, {}};
}
Task<NodeResult> ToolNode::execute(ExecutionContext &c, const Message &input) {
  auto permit = co_await limiter_.acquire(c);
  ToolContext context{c};
  ToolRequest request{input.payload};
  auto result = co_await tool_->invoke(request, context);
  c.check();
  auto message = input;
  message.payload = std::move(result.output);
  message.provenance.push_back(
      {c.node_id, tool_->metadata().name, "", "", "", input.id, "", timestamp()});
  co_return NodeResult{message, {}};
}
Task<NodeResult> RouterNode::execute(ExecutionContext &c, const Message &input) {
  c.check();
  bool accepted = input.payload.is_object() && input.payload.contains(definition_.field) &&
                  input.payload.at(definition_.field) == definition_.value;
  auto message = input;
  if (type() == "validator")
    message.provenance.push_back(
        {c.node_id, "", "", "", "", input.id, accepted ? "accepted" : "rejected", timestamp()});
  co_return NodeResult{message, accepted ? "accepted" : "rejected"};
}
void register_functions(FunctionRegistry &r) {
  r.add("identity",
        std::make_shared<Function>([](ExecutionContext &c, const Json &input) -> Task<Json> {
          c.check();
          co_return input;
        }));
  r.add("hello",
        std::make_shared<Function>([](ExecutionContext &c, const Json &input) -> Task<Json> {
          c.check();
          co_return Json{{"greeting", "Hello from LASO"}, {"input", input}};
        }));
}
} // namespace laso
