#pragma once
#include <laso/core/types.hpp>
#include <memory>

namespace laso {
enum class PolicyDecision { Allow, Deny, RequireApproval };
struct PolicyContext {
  std::string pipeline_id, node_id, resource, classification = "public";
  bool network = false, remote = false, approval_required = false;
};
struct PolicyResult {
  PolicyDecision decision;
  std::string reason;
};
class Policy {
public:
  virtual ~Policy() = default;
  virtual PolicyResult evaluate(const PolicyContext &) const = 0;
};
struct PolicyRule {
  std::string resource;
  PolicyDecision decision = PolicyDecision::Deny;
};
class PolicyEngine final : public Policy {
public:
  explicit PolicyEngine(std::vector<PolicyRule> rules = {}, bool allow_network = false)
      : rules_(std::move(rules)), allow_network_(allow_network) {}
  PolicyResult evaluate(const PolicyContext &c) const override {
    if ((c.network || c.remote) && !allow_network_)
      return {PolicyDecision::Deny, "Network access is disabled"};
    if (c.remote && c.classification != "public")
      return {PolicyDecision::Deny,
              "Non-public data cannot leave the process through remote models"};
    PolicyDecision decision =
        c.approval_required ? PolicyDecision::RequireApproval : PolicyDecision::Allow;
    for (const auto &r : rules_)
      if (r.resource == c.resource || r.resource == "*") {
        if (r.decision == PolicyDecision::Deny)
          return {r.decision, "Denied by deployment policy"};
        if (r.decision == PolicyDecision::RequireApproval)
          decision = r.decision;
      }
    return {decision, decision == PolicyDecision::RequireApproval ? "Human approval required"
                                                                  : "Allowed local operation"};
  }

private:
  std::vector<PolicyRule> rules_;
  bool allow_network_;
};
} // namespace laso
