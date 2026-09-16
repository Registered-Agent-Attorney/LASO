#include <laso/storage/factory.hpp>
#include <laso/storage/sqlite.hpp>

namespace laso {
std::unique_ptr<Storage> create_storage(const StorageOptions &options) {
  if (options.backend != "sqlite")
    throw Error(ErrorCode::Configuration, "Unsupported storage backend");
  return std::make_unique<SQLiteStorage>(options.db_path);
}
} // namespace laso
