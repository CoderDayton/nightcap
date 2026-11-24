#ifndef MOCKTAIL_RUNTIME_FAILURE_REASON_H_
#define MOCKTAIL_RUNTIME_FAILURE_REASON_H_

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace mocktail::runtime {

std::string FailureReasonFromLog(std::string_view log);

std::string ReadSettledLogTail(
    const std::filesystem::path& path, std::size_t max_bytes = 256 * 1024,
    std::chrono::milliseconds poll = std::chrono::milliseconds(100),
    std::chrono::milliseconds max_wait = std::chrono::milliseconds(1500));

}

#endif
