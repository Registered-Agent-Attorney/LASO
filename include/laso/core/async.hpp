#pragma once
// <utility> must precede Boost.Asio on the oldest supported Boost package.
#include <boost/asio.hpp>
#include <laso/core/types.hpp>
#include <stop_token>
#include <utility>

namespace laso {
namespace asio = boost::asio;
template <class T> using Task = asio::awaitable<T>;
struct ExecutionContext {
  std::string run_id, pipeline_id, node_id;
  std::stop_token stop;
  std::chrono::steady_clock::time_point deadline;
  unsigned attempt = 1, visit = 1;
  void check() const;
  Task<void> delay(Milliseconds duration) const;
};
} // namespace laso
