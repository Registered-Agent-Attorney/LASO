#pragma once
#include <cstdint>
#include <filesystem>
#include <laso/core/types.hpp>
#include <string>

namespace laso {
struct Actor {
  std::string id = "local";
  bool authenticated = false;
  std::string role = "user";
};
struct AuthorizationContext {
  Actor actor;
  std::string action, resource;
};
class IdentityProvider {
public:
  virtual ~IdentityProvider() = default;
  virtual Actor authenticate(const std::string &credential) const = 0;
  virtual Actor authenticate_request(const std::string &credential, const std::string &,
                                     const std::string &) const {
    return authenticate(credential);
  }
  virtual bool authorize(const AuthorizationContext &) const = 0;
};
class LocalDevelopmentIdentity final : public IdentityProvider {
public:
  Actor authenticate(const std::string &) const override {
    return {"local-development", true, "admin"};
  }
  bool authorize(const AuthorizationContext &) const override {
    return true;
  }
};

// Authenticates claims forwarded by a trusted LASO-Web gateway. The gateway
// token is service-to-service trust; principal and role are accepted only when
// the token matches. Deployments must restrict direct access to the Core API.
class TrustedGatewayIdentity final : public IdentityProvider {
public:
  explicit TrustedGatewayIdentity(std::string token) : token_(std::move(token)) {
    if (token_.size() < 32 || token_.size() > 8192)
      throw Error(ErrorCode::Configuration, "Invalid gateway identity token configuration");
  }
  Actor authenticate(const std::string &) const override {
    return {};
  }
  Actor authenticate_request(const std::string &credential, const std::string &principal,
                             const std::string &role) const override {
    constexpr std::string_view prefix = "Bearer ";
    if (!credential.starts_with(prefix))
      return {};
    const auto supplied = std::string_view(credential).substr(prefix.size());
    if (supplied.size() != token_.size())
      return {};
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < token_.size(); ++i)
      difference |= static_cast<std::uint8_t>(supplied[i] ^ token_[i]);
    if (difference != 0 || !valid_principal(principal) ||
        (role != "user" && role != "operator" && role != "admin"))
      return {};
    return {principal, true, role};
  }
  bool authorize(const AuthorizationContext &context) const override {
    if (!context.actor.authenticated)
      return context.action == "GET" &&
             (context.resource == "/api/v1/health" || context.resource == "/api/v1/health/live" ||
              context.resource == "/api/v1/health/ready" || context.resource == "/api/v1/version");
    if (context.resource.starts_with("/api/v1/operator/"))
      return context.actor.role == "operator" || context.actor.role == "admin";
    return true;
  }

private:
  static bool valid_principal(const std::string &value) {
    if (value.empty() || value.size() > 128)
      return false;
    const auto alnum = [](unsigned char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alnum(static_cast<unsigned char>(value.front())))
      return false;
    for (const unsigned char c : value)
      if (!alnum(c) && c != '.' && c != '_' && c != '@' && c != ':' && c != '-')
        return false;
    return true;
  }
  std::string token_;
};

class SecretProvider {
public:
  virtual ~SecretProvider() = default;
  virtual std::string resolve(const std::string &reference) const = 0;
};
class EnvironmentSecretProvider final : public SecretProvider {
public:
  std::string resolve(const std::string &reference) const override;
};
} // namespace laso
