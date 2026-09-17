#pragma once
#include <atomic>
#include <laso/events/events.hpp>
#include <laso/storage/storage.hpp>
#include <laso/workers/worker.hpp>
#include <mutex>

namespace laso {
class WorkerManager final : public EventSubscriber {
public:
  WorkerManager(Storage &, WorkerRegistry &, unsigned max_active = 32, unsigned max_per_worker = 16,
                unsigned max_artifacts = 16);
  WorkerJob submit(const WorkerRequest &);
  WorkerJob job(const std::string &) const;
  std::vector<Json> jobs(const std::string &run_id = "", std::size_t limit = 1000,
                         std::size_t offset = 0) const;
  Json workers() const;
  Json worker(const std::string &) const;
  std::string resolve_worker(const std::string &worker_id, const std::string &capability) const;
  void cancel(const std::string &, WorkerJobState requested_state, const std::string &reason);
  void receive(const Event &) override;
  void stop() noexcept;

private:
  Storage &storage_;
  WorkerRegistry &registry_;
  unsigned max_active_, max_per_worker_, max_artifacts_;
  std::atomic<bool> stopped_{false};
  mutable std::mutex submit_mutex_;
  void apply_event(const Event &);
  WorkerJob reconcile(WorkerJob);
  void persist(WorkerJob &);
  static std::string durable_job_id(const std::string &idempotency_key);
  static WorkerJobState parse_state(const Json &,
                                    WorkerJobState fallback = WorkerJobState::Unknown);
};
} // namespace laso
