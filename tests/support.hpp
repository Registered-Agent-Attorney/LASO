#pragma once
#include <gtest/gtest.h>
#include <laso/application/service.hpp>
#include <laso/pipeline/parser.hpp>
#include <laso/storage/sqlite.hpp>

namespace laso::test {
struct TemporaryDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() / ("laso-test-" + uuid());
  TemporaryDirectory() {
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
inline Config config(const std::filesystem::path &dir) {
  Config c;
  c.data_dir = dir;
  c.validate();
  return c;
}
inline std::unique_ptr<Storage> make_storage(const std::filesystem::path &path) {
  return std::make_unique<SQLiteStorage>(path);
}
inline std::string fixture(const std::string &name) {
  return read_document(std::filesystem::path(LASO_SOURCE_DIR) / "examples" / name /
                       "pipeline.yaml");
}
inline std::string single(const std::string &node = "type: function\n    function: identity",
                          const std::string &extra = "") {
  return "laso: '1'\nname: test\nversion: 1\n" + extra + "nodes:\n  action:\n    " + node +
         "\nedges:\n  - {from: input, to: action}\n  - {from: action, to: output}\n";
}
inline laso::Run execute(Service &service, asio::io_context &io, const std::string &yaml,
                         Json input = Json::object()) {
  auto name = service.register_pipeline(yaml).at("name").get<std::string>();
  auto id = service.start(name, input);
  io.restart();
  io.run();
  return service.get(RecordKind::Run, id).get<laso::Run>();
}
} // namespace laso::test
