#pragma once
#include <laso/core/types.hpp>
#include <algorithm>
#include <memory>

namespace laso {
enum class PolicyDecision { Allow, Deny, RequireApproval };
struct PolicyContext {
  std::string pipeline_id, node_id, resource, classification = "public";
  bool network = false, remote = false, worker_node = false, approval_required = false;
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
  explicit PolicyEngine(std::vector<PolicyRule> rules = {}, bool allow_network = false,
                        std::vector<std::string> allow_remote_workers = {})
      : rules_(std::move(rules)), allow_network_(allow_network),
        allow_remote_workers_(std::move(allow_remote_workers)) {}
  PolicyResult evaluate(const PolicyContext &c) const override {
    const bool trusted_remote_worker =
        c.remote && c.worker_node &&
        std::find(allow_remote_workers_.begin(), allow_remote_workers_.end(), c.resource) !=
            allow_remote_workers_.end();
    if ((c.network || c.remote) && !allow_network_ && !trusted_remote_worker)
      return {PolicyDecision::Deny, "Network access is disabled"};
    if (c.remote && !c.worker_node && c.classification != "public")
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
  std::vector<std::string> allow_remote_workers_;
};
} // namespace laso
