#include <cstdlib>
#include <fcntl.h>
#include <laso/security/security.hpp>
#include <regex>
#include <sys/file.h>
#include <unistd.h>

namespace laso {
std::string EnvironmentSecretProvider::resolve(const std::string &reference) const {
  static const std::regex valid("[A-Z_][A-Z0-9_]{0,127}");
  if (!std::regex_match(reference, valid))
    throw Error(ErrorCode::Configuration, "Invalid secret reference");
  auto *value = std::getenv(reference.c_str());
  if (!value)
    throw Error(ErrorCode::Configuration, "Required secret unavailable");
  return value;
}
ProcessLease::ProcessLease(const std::filesystem::path &database) {
  if (database.has_parent_path())
    std::filesystem::create_directories(database.parent_path());
  auto path = database;
  path += ".lock";
  fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0)
    throw Error(ErrorCode::Storage, "Cannot open database process lease");
  if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    close(fd_);
    fd_ = -1;
    throw Error(ErrorCode::Conflict, "Database is owned by another LASO process; use its API or "
                                     "stop it before local CLI access");
  }
}
ProcessLease::~ProcessLease() {
  if (fd_ >= 0) {
    flock(fd_, LOCK_UN);
    close(fd_);
  }
}
} // namespace laso
