#include <laso/core/config.hpp>
#include <laso/scheduler/scheduler.hpp>

namespace laso {
void LocalScheduler::schedule(ScheduledPipeline s) {
  std::lock_guard lock(mutex_);
  if (stopped_)
    throw Error(ErrorCode::Conflict, "Scheduler is stopped");
  if (s.max_firings == 0 || s.max_firings > 100000 || s.delay < Milliseconds{0} ||
      s.interval < Milliseconds{0} || (s.max_firings > 1 && s.interval < Milliseconds{1}) ||
      timers_.size() >= 128)
    throw Error(ErrorCode::Configuration, "Invalid schedule or scheduler capacity exceeded");
  auto timer = std::make_shared<asio::steady_timer>(strand_);
  if (!timers_.emplace(s.id, timer).second)
    throw Error(ErrorCode::Conflict, "Duplicate schedule");
  asio::co_spawn(
      strand_,
      [this, s, timer]() -> Task<void> {
        for (unsigned i = 0; i < s.max_firings; ++i) {
          if (stopped_)
            break;
          timer->expires_after(i == 0 ? s.delay : s.interval);
          boost::system::error_code ec;
          co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
          if (ec || stopped_)
            break;
          try {
            dispatch_(s);
          } catch (...) {
            log_diagnostic("schedule.dispatch_failed", {{"schedule_id", s.id}});
          }
        }
        std::lock_guard guard(mutex_);
        timers_.erase(s.id);
      },
      asio::detached);
}
void LocalScheduler::stop() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
  for (auto &[id, timer] : timers_) {
    (void)id;
    asio::post(strand_, [timer] { timer->cancel(); });
  }
}
} // namespace laso
