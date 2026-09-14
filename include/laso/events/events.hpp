#pragma once
#include <functional>
#include <laso/core/types.hpp>
#include <memory>
#include <mutex>

namespace laso {
class EventSubscriber {
public:
  virtual ~EventSubscriber() = default;
  virtual void receive(const Event &) = 0;
};
class EventBus {
public:
  virtual ~EventBus() = default;
  virtual void publish(const Event &) noexcept = 0;
  virtual void subscribe(std::shared_ptr<EventSubscriber>) = 0;
};
class InProcessEventBus final : public EventBus {
public:
  void subscribe(std::shared_ptr<EventSubscriber> s) override {
    std::lock_guard lock(mutex_);
    subscribers_.push_back(std::move(s));
  }
  void publish(const Event &e) noexcept override {
    try {
      std::vector<std::shared_ptr<EventSubscriber>> copy;
      {
        std::lock_guard lock(mutex_);
        copy = subscribers_;
      }
      for (const auto &s : copy) {
        try {
          s->receive(e);
        } catch (...) { /* Subscriber failures cannot roll back committed state. */
        }
      }
    } catch (...) { /* Durable events remain available for replay. */
    }
  }

private:
  std::mutex mutex_;
  std::vector<std::shared_ptr<EventSubscriber>> subscribers_;
};
class TelemetryExporter : public EventSubscriber {};
} // namespace laso
