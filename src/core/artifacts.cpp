#include <cerrno>
#include <fcntl.h>
#include <laso/artifacts/artifacts.hpp>
#include <unistd.h>

namespace laso {
Artifact LocalArtifactStore::put(Artifact a, std::span<const std::byte> bytes) {
  if (bytes.size() > 16 * 1024 * 1024)
    throw Error(ErrorCode::Validation, "Artifact exceeds 16 MiB");
  std::filesystem::create_directories(root_);
  // Never incorporate untrusted artifact names, IDs or run IDs into a filesystem path.
  a.id = uuid();
  auto target = root_ / a.id;
  int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw Error(ErrorCode::Storage, "Cannot create artifact");
  try {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        throw Error(ErrorCode::Storage, "Artifact write failed");
      offset += static_cast<std::size_t>(written);
    }
    if (fsync(fd) != 0)
      throw Error(ErrorCode::Storage, "Artifact flush failed");
    close(fd);
    fd = -1;
    int directory_fd = open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0)
      throw Error(ErrorCode::Storage, "Cannot open artifact directory");
    auto sync_result = fsync(directory_fd);
    close(directory_fd);
    if (sync_result != 0)
      throw Error(ErrorCode::Storage, "Artifact directory flush failed");
    a.location = std::filesystem::absolute(target).string();
    storage_.commit({{RecordKind::Artifact, a.id, a.run_id, Json(a)}});
    return a;
  } catch (...) {
    if (fd >= 0)
      close(fd);
    std::error_code ec;
    std::filesystem::remove(target, ec);
    throw;
  }
}
} // namespace laso
