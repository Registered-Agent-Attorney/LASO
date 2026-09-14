#pragma once
#include <laso/core/async.hpp>
#include <laso/core/registry.hpp>

namespace laso {
struct ToolMetadata {
  std::string name, description, permission = "local", plugin;
  Json input_schema = Json::object(), output_schema = Json::object();
  Milliseconds timeout{30000};
  bool approval_required = false, network = false;
};
struct ToolRequest {
  Json input = Json::object();
};
struct ToolResult {
  Json output = Json::object();
};
struct ToolContext {
  ExecutionContext &execution;
};
class Tool {
public:
  virtual ~Tool() = default;
  virtual ToolMetadata metadata() const = 0;
  virtual Task<ToolResult> invoke(const ToolRequest &, ToolContext &) = 0;
};
using ToolRegistry = Registry<Tool>;
class EchoTool final : public Tool {
public:
  ToolMetadata metadata() const override {
    ToolMetadata result;
    result.name = "echo";
    result.description = "Return structured input unchanged";
    return result;
  }
  Task<ToolResult> invoke(const ToolRequest &r, ToolContext &c) override {
    c.execution.check();
    co_return ToolResult{r.input};
  }
};
} // namespace laso
