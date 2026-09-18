#pragma once

#include <cstddef>
#include <cstdint>
#include <laso/core/types.hpp>
#include <memory>
#include <optional>
#include <string>

namespace laso {
struct CoordinationOptions {
  std::string backend = "postgres";
  std::string postgres_dsn;
  std::string postgres_schema = "public";
  std::size_t pool_min_connections = 1;
  std::size_t pool_max_connections = 4;
  std::uint64_t pool_acquisition_timeout_ms = 1000;
};

struct LeaseRecord {
  std::string resource_key;
  std::string owner_instance;
  std::uint64_t fencing_token = 0;
  std::string acquired_at;
  std::string heartbeat_at;
  std::string expires_at;
  bool active = false;
};

struct CoordinationDiagnostics {
  std::size_t pool_size = 0;
  std::size_t pool_in_use = 0;
  std::uint64_t pool_acquisition_timeouts = 0;
  std::uint64_t pool_replacements = 0;
  std::uint64_t lease_acquisition_failures = 0;
  std::uint64_t renewal_failures = 0;
  std::uint64_t fencing_rejections = 0;
};

class Coordination {
public:
  virtual ~Coordination() = default;
  virtual std::optional<LeaseRecord> acquire(const std::string &resource_key,
                                             std::uint64_t ttl_ms) = 0;
  virtual bool renew(LeaseRecord &, std::uint64_t ttl_ms) = 0;
  virtual bool release(const LeaseRecord &) = 0;
  virtual std::optional<LeaseRecord> inspect(const std::string &resource_key) const = 0;
  // Throws Conflict if the lease no longer owns the current fencing token.
  // Protected writes must include the same token predicate in their own SQL.
  virtual void require_current(const LeaseRecord &) = 0;
  virtual CoordinationDiagnostics diagnostics() const = 0;
};

std::unique_ptr<Coordination> create_coordination(const CoordinationOptions &,
                                                  const std::string &owner_instance);

// A fresh opaque identifier for one running LASO service instance. It is not
// derived from host, user, network, or hardware identity.
inline std::string generate_service_instance_id() {
  return uuid();
}
} // namespace laso
