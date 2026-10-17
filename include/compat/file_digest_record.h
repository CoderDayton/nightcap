#ifndef MOCKTAIL_COMPAT_FILE_DIGEST_RECORD_H_
#define MOCKTAIL_COMPAT_FILE_DIGEST_RECORD_H_

#include <filesystem>
#include <functional>
#include <string>

namespace mocktail::compat {

inline constexpr char kFileDigestRecordDirectory[] = "file-digests";

std::string RememberedFileDigest(
    const std::filesystem::path& record_directory,
    const std::filesystem::path& path,
    const std::function<std::string()>& hash);

}

#endif
