#pragma once
#include <atomic>
#include <functional>
#include <laso/core/async.hpp>
#include <memory>
#include <mutex>

namespace laso {
struct ScheduledPipeline {
  std::string id = uuid(), pipeline_id;
  Milliseconds delay{0}, interval{0};
  unsigned max_firings = 1;
  Json input = Json::object();
};
class Scheduler {
public:
  virtual ~Scheduler() = default;
  virtual void schedule(ScheduledPipeline) = 0;
  virtual void stop() = 0;
};
class LocalScheduler final : public Scheduler {
public:
  LocalScheduler(asio::io_context &io, std::function<void(const ScheduledPipeline &)> dispatch)
      : strand_(asio::make_strand(io)), dispatch_(std::move(dispatch)) {}
  void schedule(ScheduledPipeline) override;
  void stop() override;

private:
  asio::strand<asio::io_context::executor_type> strand_;
  std::function<void(const ScheduledPipeline &)> dispatch_;
  std::mutex mutex_;
  std::atomic<bool> stopped_{false};
  std::map<std::string, std::shared_ptr<asio::steady_timer>> timers_;
};
} // namespace laso
