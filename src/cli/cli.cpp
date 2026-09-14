#include <CLI/CLI.hpp>
#include <iostream>
#include <laso/application/service.hpp>
#include <laso/cli/cli.hpp>
#include <laso/pipeline/parser.hpp>
#include <sys/stat.h>

namespace laso {
int cli_main(int argc, char **argv) {
  umask(0077);
  CLI::App app{"LASO native orchestration framework"};
  std::string config_path, data_dir, target, input = "{}", comment, actor = "local";
  app.add_option("--config", config_path, "Configuration file");
  app.add_option("--data-dir", data_dir, "Override data directory");
  auto *version_command = app.add_subcommand("version");
  auto *health = app.add_subcommand("health", "Check local storage initialization");
  auto *pipeline = app.add_subcommand("pipeline");
  pipeline->require_subcommand(1);
  auto *validate = pipeline->add_subcommand("validate");
  validate->add_option("file", target)->required();
  auto *register_command = pipeline->add_subcommand("register");
  register_command->add_option("file", target)->required();
  auto *pipeline_list = pipeline->add_subcommand("list");
  auto *pipeline_show = pipeline->add_subcommand("show");
  pipeline_show->add_option("name", target)->required();
  auto *run = app.add_subcommand("run");
  run->require_subcommand(1);
  auto *start = run->add_subcommand("start");
  start->add_option("pipeline", target)->required();
  start->add_option("--input", input, "JSON payload");
  start->add_option("--actor", actor);
  auto *run_list = run->add_subcommand("list");
  auto *show = run->add_subcommand("show");
  show->add_option("id", target)->required();
  auto *cancel = run->add_subcommand("cancel");
  cancel->add_option("id", target)->required();
  auto *resume = run->add_subcommand("resume");
  resume->add_option("id", target)->required();
  auto *approval = app.add_subcommand("approval");
  approval->require_subcommand(1);
  auto *approval_list = approval->add_subcommand("list");
  auto *approve = approval->add_subcommand("approve");
  auto *reject = approval->add_subcommand("reject");
  for (auto *command : {approve, reject}) {
    command->add_option("id", target)->required();
    command->add_option("--actor", actor);
    command->add_option("--comment", comment);
  }
  auto *plugin = app.add_subcommand("plugin");
  plugin->require_subcommand(1);
  auto *plugin_list = plugin->add_subcommand("list");
  auto *provider = app.add_subcommand("provider");
  provider->require_subcommand(1);
  auto *provider_list = provider->add_subcommand("list");
  auto *tool = app.add_subcommand("tool");
  tool->require_subcommand(1);
  auto *tool_list = tool->add_subcommand("list");
  app.require_subcommand(1);
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }
  try {
    if (*version_command) {
      std::cout << version << '\n';
      return 0;
    }
    if (*validate) {
      auto p = parse_pipeline(read_document(target));
      std::cout << Json{{"valid", true}, {"name", p.name}}.dump(2) << '\n';
      return 0;
    }
    std::map<std::string, std::string> overrides;
    if (!data_dir.empty())
      overrides["data_dir"] = data_dir;
    auto config = load_config(config_path, overrides);
    Executor executor(config.workers);
    Service service(executor.context(), config);
    Json result;
    std::string run_id;
    if (*health)
      result = {{"status", "ok"}, {"scope", "local storage; not a daemon probe"}};
    else if (*register_command)
      result = service.register_pipeline(read_document(target));
    else if (*pipeline_list)
      result = service.list(RecordKind::Pipeline);
    else if (*pipeline_show)
      result = service.get(RecordKind::Pipeline, target);
    else if (*run_list)
      result = service.list(RecordKind::Run);
    else if (*show)
      result = service.get(RecordKind::Run, target);
    else if (*start)
      run_id = service.start(target, Json::parse(input), actor, true);
    else if (*cancel) {
      service.runtime().cancel(target);
      result = service.get(RecordKind::Run, target);
    } else if (*resume) {
      service.runtime().resume(target);
      run_id = target;
    } else if (*approval_list)
      result = service.list(RecordKind::Approval);
    else if (*approve || *reject) {
      run_id = service.get(RecordKind::Approval, target).at("run_id").get<std::string>();
      service.runtime().decide(target, static_cast<bool>(*approve), actor, comment);
    } else if (*plugin_list)
      result = service.plugins();
    else if (*provider_list)
      result = service.providers();
    else if (*tool_list)
      result = service.tools();
    executor.start();
    executor.join();
    if (!run_id.empty())
      result = service.get(RecordKind::Run, run_id);
    std::cout << result.dump(2) << '\n';
    return result.is_object() && (result.value("state", std::string{}) == "Failed" ||
                                  result.value("state", std::string{}) == "TimedOut")
               ? 2
               : 0;
  } catch (const Error &e) {
    std::cerr << e.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "Invalid input or operation failed\n";
    return 2;
  }
}
} // namespace laso
