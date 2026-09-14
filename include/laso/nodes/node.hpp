#pragma once
#include <functional>
#include <laso/providers/provider.hpp>
#include <laso/runtime/executor.hpp>
#include <laso/tools/tool.hpp>

namespace laso {
struct NodeResult {
  Message message;
  std::string condition;
};
class Node {
public:
  virtual ~Node() = default;
  virtual Task<NodeResult> execute(ExecutionContext &, const Message &) = 0;
  virtual std::string_view type() const noexcept = 0;
};
using Function = std::function<Task<Json>(ExecutionContext &, const Json &)>;
using FunctionRegistry = Registry<Function>;
class PassthroughNode : public Node {
public:
  Task<NodeResult> execute(ExecutionContext &c, const Message &m) override {
    c.check();
    co_return NodeResult{m, {}};
  }
};
class InputNode final : public PassthroughNode {
public:
  std::string_view type() const noexcept override {
    return "input";
  }
};
class OutputNode final : public PassthroughNode {
public:
  std::string_view type() const noexcept override {
    return "output";
  }
};
// Structural semantics are owned by the shared runtime/checkpoint engine.
class ApprovalNode final : public PassthroughNode {
public:
  std::string_view type() const noexcept override {
    return "approval";
  }
};
class ParallelNode final : public PassthroughNode {
public:
  std::string_view type() const noexcept override {
    return "parallel";
  }
};
class JoinNode final : public PassthroughNode {
public:
  std::string_view type() const noexcept override {
    return "join";
  }
};
class SubpipelineNode final : public PassthroughNode {
public:
  std::string_view type() const noexcept override {
    return "subpipeline";
  }
};
class FunctionNode final : public Node {
public:
  explicit FunctionNode(std::shared_ptr<Function> function) : function_(std::move(function)) {}
  Task<NodeResult> execute(ExecutionContext &, const Message &) override;
  std::string_view type() const noexcept override {
    return "function";
  }

private:
  std::shared_ptr<Function> function_;
};
class AgentNode final : public Node {
public:
  AgentNode(std::shared_ptr<ModelProvider> provider, ModelBinding binding, std::string prompt,
            AsyncLimiter &limiter)
      : provider_(std::move(provider)), binding_(std::move(binding)), prompt_(std::move(prompt)),
        limiter_(limiter) {}
  Task<NodeResult> execute(ExecutionContext &, const Message &) override;
  std::string_view type() const noexcept override {
    return "agent";
  }

private:
  std::shared_ptr<ModelProvider> provider_;
  ModelBinding binding_;
  std::string prompt_;
  AsyncLimiter &limiter_;
};
class ToolNode final : public Node {
public:
  ToolNode(std::shared_ptr<Tool> tool, AsyncLimiter &limiter)
      : tool_(std::move(tool)), limiter_(limiter) {}
  Task<NodeResult> execute(ExecutionContext &, const Message &) override;
  std::string_view type() const noexcept override {
    return "tool";
  }

private:
  std::shared_ptr<Tool> tool_;
  AsyncLimiter &limiter_;
};
class RouterNode : public Node {
public:
  explicit RouterNode(NodeDefinition definition) : definition_(std::move(definition)) {}
  Task<NodeResult> execute(ExecutionContext &, const Message &) override;
  std::string_view type() const noexcept override {
    return "router";
  }

protected:
  NodeDefinition definition_;
};
class ValidatorNode final : public RouterNode {
public:
  using RouterNode::RouterNode;
  std::string_view type() const noexcept override {
    return "validator";
  }
};
class LoopNode final : public Node {
public:
  explicit LoopNode(unsigned maximum) : maximum_(maximum) {}
  Task<NodeResult> execute(ExecutionContext &c, const Message &m) override {
    c.check();
    co_return NodeResult{m, c.visit <= maximum_ ? "repeat" : "done"};
  }
  std::string_view type() const noexcept override {
    return "loop";
  }

private:
  unsigned maximum_;
};
using NodeFactory = std::function<std::unique_ptr<Node>(const NodeDefinition &)>;
using NodeRegistry = Registry<NodeFactory>;
void register_functions(FunctionRegistry &registry);
} // namespace laso
