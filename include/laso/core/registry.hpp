#pragma once
#include <laso/core/types.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace laso {
// Values are immutable registrations; implementations own their internal synchronization.
template <class T> class Registry {
public:
  void add_batch(const std::map<std::string, std::shared_ptr<T>> &batch) {
    std::unique_lock lock(mutex_);
    auto candidate = entries_;
    for (const auto &[name, value] : batch) {
      if (name.empty() || !value || !candidate.emplace(name, value).second)
        throw Error(ErrorCode::Conflict, "Invalid or duplicate registration");
    }
    entries_.swap(candidate);
  }
  void add(const std::string &name, std::shared_ptr<T> value) {
    if (name.empty() || !value)
      throw Error(ErrorCode::Configuration, "Invalid registration");
    std::unique_lock lock(mutex_);
    if (!entries_.emplace(name, std::move(value)).second)
      throw Error(ErrorCode::Conflict, "Duplicate registration");
  }
  std::shared_ptr<T> get(const std::string &name) const {
    std::shared_lock lock(mutex_);
    auto found = entries_.find(name);
    if (found == entries_.end())
      throw Error(ErrorCode::NotFound, "Registration not found");
    return found->second;
  }
  std::vector<std::string> names() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> result;
    for (const auto &[name, value] : entries_) {
      (void)value;
      result.push_back(name);
    }
    return result;
  }

private:
  mutable std::shared_mutex mutex_;
  std::map<std::string, std::shared_ptr<T>> entries_;
};
} // namespace laso
