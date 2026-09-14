#pragma once
#include <laso/storage/storage.hpp>
#include <span>

namespace laso {
class ArtifactStore {
public:
  virtual ~ArtifactStore() = default;
  virtual Artifact put(Artifact metadata, std::span<const std::byte> bytes) = 0;
};
class LocalArtifactStore final : public ArtifactStore {
public:
  LocalArtifactStore(std::filesystem::path root, Storage &storage)
      : root_(std::move(root)), storage_(storage) {}
  Artifact put(Artifact metadata, std::span<const std::byte> bytes) override;

private:
  std::filesystem::path root_;
  Storage &storage_;
};
} // namespace laso
