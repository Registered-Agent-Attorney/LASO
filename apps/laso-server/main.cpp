#include <CLI/CLI.hpp>
#include <csignal>
#include <iostream>
#include <laso/api/api.hpp>
#include <sys/stat.h>

int main(int argc, char **argv) {
  CLI::App app{"LASO server"};
  std::string config_path, host;
  unsigned port = 0;
  app.add_option("--config", config_path);
  app.add_option("--host", host);
  app.add_option("--port", port);
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }
  try {
    umask(0077);
    std::map<std::string, std::string> overrides;
    if (!host.empty())
      overrides["api_host"] = host;
    if (port)
      overrides["api_port"] = std::to_string(port);
    auto config = laso::load_config(config_path, overrides);
    laso::Executor executor(config.workers);
    laso::Service service(executor.context(), config);
    laso::LocalDevelopmentIdentity identity;
    laso::Api api(service, identity);
    laso::HttpServer server(executor.context(), api, config.api_host,
                            static_cast<unsigned short>(config.api_port));
    laso::asio::signal_set signals(executor.context(), SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code &, int) {
      server.stop();
      service.shutdown();
    });
    server.start();
    executor.start();
    executor.join();
    return 0;
  } catch (const laso::Error &e) {
    std::cerr << e.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "LASO server initialization failed\n";
    return 1;
  }
}
