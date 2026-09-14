#pragma once
#include <atomic>
#include <laso/core/async.hpp>
#include <thread>

namespace laso {
// Slots are acquired without blocking a worker. Queue capacity is bounded by max_runs.
class AsyncLimiter {
public:
  class Permit {
  public:
    explicit Permit(AsyncLimiter &owner) : owner_(&owner) {}
    Permit(Permit &&other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
    Permit(const Permit &) = delete;
    Permit &operator=(const Permit &) = delete;
    ~Permit() {
      if (owner_)
        --owner_->active_;
    }

  private:
    AsyncLimiter *owner_;
  };
  explicit AsyncLimiter(unsigned limit) : limit_(limit) {}
  Task<Permit> acquire(const ExecutionContext &context);

private:
  std::atomic<unsigned> active_{0};
  unsigned limit_;
};
class Executor {
public:
  explicit Executor(unsigned workers);
  ~Executor();
  asio::io_context &context() {
    return context_;
  }
  void start();
  void join();

private:
  asio::io_context context_;
  unsigned count_;
  std::vector<std::jthread> threads_;
};
} // namespace laso
