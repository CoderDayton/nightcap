#ifndef MOCKTAIL_RUNTIME_ROBLOX_MOUSE_BEHAVIOR_QUERY_H_
#define MOCKTAIL_RUNTIME_ROBLOX_MOUSE_BEHAVIOR_QUERY_H_

#include <cstddef>
#include <cstdint>
#include <optional>

namespace mocktail::runtime {

std::optional<std::size_t> FindLockCenterCompare(const std::uint8_t* code,
                                                 std::size_t size);

std::uint8_t* LocateLockCenterCompare(void* function, const std::uint8_t* begin,
                                      const std::uint8_t* end);

std::uint8_t* PrepareLockCenterCompare(void* function);

}

#endif
