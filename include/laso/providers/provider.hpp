#pragma once
#include <laso/core/async.hpp>
#include <laso/core/registry.hpp>

namespace laso {
struct ProviderMetadata {
  std::string name, version, plugin;
  std::size_t context_size = 4096;
  bool remote = false, streaming = false, network = false;
  Milliseconds timeout{30000};
  std::vector<std::string> capabilities{"structured-output"};
};
struct ModelRequest {
  std::string model, prompt;
  Json input = Json::object(), options = Json::object();
};
struct ModelResponse {
  Json output = Json::object();
  std::string model, provider;
};
struct ProviderHealth {
  bool healthy;
  std::string detail;
};
class ModelProvider {
public:
  virtual ~ModelProvider() = default;
  virtual Task<ModelResponse> generate(const ModelRequest &, ExecutionContext &) = 0;
  virtual ProviderHealth health() const = 0;
  virtual ProviderMetadata metadata() const = 0;
};
class MockModelProvider final : public ModelProvider {
public:
  Task<ModelResponse> generate(const ModelRequest &r, ExecutionContext &c) override {
    c.check();
    Json output = r.input;
    if (!output.is_object())
      output = Json{{"input", output}};
    output["reviewed"] = true;
    output["text"] = "Offline mock response";
    co_return ModelResponse{output, r.model, "mock"};
  }
  ProviderHealth health() const override {
    return {true, "offline"};
  }
  ProviderMetadata metadata() const override {
    return {.name = "mock", .version = "1", .plugin = ""};
  }
};
// An optional adapter for an OpenAI-compatible service bound to the local host.
// It deliberately accepts only clear-text loopback endpoints; TLS termination and
// remote provider adapters belong outside the v0.1 local baseline.
class LocalOpenAICompatibleProvider final : public ModelProvider {
public:
  explicit LocalOpenAICompatibleProvider(const std::string &endpoint);
  Task<ModelResponse> generate(const ModelRequest &, ExecutionContext &) override;
  ProviderHealth health() const override;
  ProviderMetadata metadata() const override;

private:
  std::string host_, port_;
};
using ProviderRegistry = Registry<ModelProvider>;
struct ModelBinding {
  std::string provider, model;
  Json options = Json::object();
};
} // namespace laso
