#pragma once
#include <laso/application/service.hpp>

namespace laso {
struct ApiResponse {
  unsigned status = 200;
  Json body = Json::object();
};
class Api {
public:
  Api(Service &service, IdentityProvider &identity) : service_(service), identity_(identity) {}
  ApiResponse handle(const std::string &method, const std::string &target, const std::string &body,
                     const std::string &credential = "") noexcept;

private:
  Service &service_;
  IdentityProvider &identity_;
  ApiResponse route(const std::string &, const std::string &, const Json &, const Actor &,
                    std::size_t limit, std::size_t offset, std::uint64_t after);
};
class HttpServer {
public:
  HttpServer(asio::io_context &, Api &, const std::string &host, unsigned short port);
  ~HttpServer();
  void start();
  void stop();
  unsigned short port() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace laso
