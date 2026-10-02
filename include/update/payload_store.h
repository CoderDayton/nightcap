#ifndef MOCKTAIL_UPDATE_PAYLOAD_STORE_H_
#define MOCKTAIL_UPDATE_PAYLOAD_STORE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mocktail::update {

struct PayloadStoreResult {
  std::string payload_id;
  std::filesystem::path payload_directory;
  std::filesystem::path host_abi_profile;
  std::filesystem::path compatibility_manifest;
  std::filesystem::path approval_receipt;
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string build_id;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !payload_id.empty();
  }
};

struct PayloadGarbageResult {
  std::vector<std::string> removed;
  std::uintmax_t freed_bytes = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Optional per-store fault hook for atomic manifest writes. Returning true
// makes the selected operation fail through its normal cleanup path.
enum class ManifestWritePhase { kCreate, kWrite, kFileFsync, kRename };
using ManifestWriteFault =
    std::function<bool(const std::filesystem::path&, ManifestWritePhase)>;

class PayloadStore final {
 public:
  PayloadStore(std::filesystem::path root,
               std::filesystem::path compatibility_manifest,
               std::filesystem::path runtime_binary = {},
               ManifestWriteFault manifest_write_fault = {});

  PayloadStoreResult Stage(const std::filesystem::path& prepared_payload);
  PayloadStoreResult Promote(std::string_view payload_id);
  PayloadStoreResult PromoteProbation(
      std::string_view payload_id,
      const std::filesystem::path& candidate_profile,
      const std::filesystem::path& candidate_compatibility,
      const std::array<std::filesystem::path, 2>& canary_logs);
  PayloadStoreResult Import(const std::filesystem::path& prepared_payload);
  PayloadStoreResult InspectCurrent() const;
  PayloadStoreResult VerifyCurrent();
  PayloadStoreResult Rollback();
  // Deletes payload trees nothing points at any more. The active payload, the
  // rollback target, and every ID in `keep` survive; a payload tree is around
  // 600 MiB, so without this the store grows by one release forever.
  PayloadGarbageResult CollectGarbage(const std::vector<std::string>& keep);
  std::string StatusJson(std::string* error = nullptr) const;

 private:
  std::filesystem::path root_;
  std::filesystem::path compatibility_manifest_;
  std::filesystem::path runtime_binary_;
  ManifestWriteFault manifest_write_fault_;
};

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_PAYLOAD_STORE_H_
