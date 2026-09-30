#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <laso/application/service.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace laso {
struct ApiResponse {
  unsigned status = 200;
  Json body = Json::object();
  std::string content_type = "application/json";
  std::optional<std::string> raw_body = std::nullopt;
  std::optional<unsigned> retry_after = std::nullopt;
};
class Api {
public:
  Api(Service &service, IdentityProvider &identity) : service_(service), identity_(identity) {}
  ApiResponse handle(const std::string &method, const std::string &target, const std::string &body,
                     const std::string &credential = "", const std::string &principal = "",
                     const std::string &role = "") noexcept;

private:
  Service &service_;
  IdentityProvider &identity_;
  std::array<std::array<std::atomic_uint64_t, 4>, 6> request_counts_{};
  std::array<std::atomic_uint64_t, 10> request_duration_buckets_{};
  std::atomic_uint64_t request_duration_count_{0};
  std::atomic_uint64_t request_duration_nanoseconds_{0};
  void record_request(std::string_view method, unsigned status,
                      std::chrono::steady_clock::duration duration) noexcept;
  std::string prometheus_metrics() const;
  ApiResponse route(const std::string &, const std::string &, const Json &, const Actor &,
                    std::size_t limit, std::size_t offset, std::uint64_t after,
                    std::optional<std::uint64_t> before);
};
struct HttpServerOptions {
  std::size_t max_session_streams = 32;
  std::chrono::seconds heartbeat_interval{15};
  std::chrono::seconds max_session_stream_lifetime{30 * 60};
};
struct HttpServerMetrics {
  std::size_t active_session_streams = 0;
  std::uint64_t accepted_session_streams = 0;
  std::uint64_t rejected_session_streams = 0;
  std::uint64_t closed_session_streams = 0;
  std::size_t session_stream_limit = 32;
};
class HttpServer {
public:
  HttpServer(asio::io_context &, Api &, const std::string &host, unsigned short port,
             HttpServerOptions options = {});
  ~HttpServer();
  void start();
  void stop();
  unsigned short port() const;
  HttpServerMetrics metrics() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace laso
