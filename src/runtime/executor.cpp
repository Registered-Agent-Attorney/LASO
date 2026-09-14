#include <algorithm>
#include <laso/runtime/executor.hpp>

namespace laso {
void ExecutionContext::check() const {
  if (stop.stop_requested())
    throw Error(ErrorCode::Cancellation, "Execution cancelled");
  if (std::chrono::steady_clock::now() >= deadline)
    throw Error(ErrorCode::Timeout, "Execution deadline exceeded");
}
Task<void> ExecutionContext::delay(Milliseconds duration) const {
  auto end = std::chrono::steady_clock::now() + duration;
  auto executor = co_await asio::this_coro::executor;
  while (std::chrono::steady_clock::now() < end) {
    check();
    asio::steady_timer timer(executor);
    timer.expires_at(
        std::min({end, deadline, std::chrono::steady_clock::now() + Milliseconds{10}}));
    co_await timer.async_wait(asio::use_awaitable);
  }
  check();
}
Task<AsyncLimiter::Permit> AsyncLimiter::acquire(const ExecutionContext &c) {
  for (;;) {
    c.check();
    auto value = active_.load();
    if (value < limit_ && active_.compare_exchange_weak(value, value + 1))
      co_return Permit(*this);
    co_await c.delay(Milliseconds{5});
  }
}
Executor::Executor(unsigned workers) : count_(workers) {
  if (workers == 0 || workers > 64)
    throw Error(ErrorCode::Configuration, "Worker count outside bounds");
}
Executor::~Executor() {
  context_.stop();
  join();
}
void Executor::start() {
  if (!threads_.empty())
    throw Error(ErrorCode::Conflict, "Executor already started");
  for (unsigned i = 0; i < count_; ++i)
    threads_.emplace_back([this] { context_.run(); });
}
void Executor::join() {
  for (auto &thread : threads_)
    if (thread.joinable())
      thread.join();
  threads_.clear();
}
} // namespace laso
