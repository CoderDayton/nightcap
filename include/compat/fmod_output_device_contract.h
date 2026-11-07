#ifndef MOCKTAIL_COMPAT_FMOD_OUTPUT_DEVICE_CONTRACT_H_
#define MOCKTAIL_COMPAT_FMOD_OUTPUT_DEVICE_CONTRACT_H_

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace mocktail::compat {

// The 2.738 integer-index selector adds a feature-flag dispatch before the
// count/clamp/setDriver path. Keep the calling convention, count slot (5),
// object offset (0x28), and branch into that path exact. Only relocated
// addresses and non-local branch/call targets vary between builds.
inline constexpr char kFmodDeviceListsSelectContract[] =
    "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x50\x41"
    "\x89\xf7\x49\x89\xfe\x4c\x8b\x25\x00\x00\x00\x00\x49\x8b\x04\x24"
    "\x48\x89\x45\xd8\x80\x3d\x00\x00\x00\x00\x00\x74\x25\x49\x8b\x04"
    "\x24\x48\x3b\x45\xd8\x0f\x85\x00\x00\x00\x00\x4c\x89\xf7\x44\x89"
    "\xfe\x48\x83\xc4\x50\x5b\x41\x5c\x41\x5e\x41\x5f\x5d\xe9\x00\x00"
    "\x00\x00\x49\x8b\x06\x4c\x89\xf7\xff\x50\x28\x44\x39\xf8\x0f\x8e"
    "\x00\x00\x00\x00\x31\xdb\x45\x85\xff\x41\x0f\x4f\xdf\x49\x8b\x7e"
    "\x28\x89\xde\xe8\x00\x00\x00\x00";
inline constexpr std::size_t kFmodDeviceListsSelectContractSize =
    sizeof(kFmodDeviceListsSelectContract) - 1;

// The same selector from 2.746, which ORs two feature flags where 2.738
// tested one.
inline constexpr char kFmodDeviceListsSelectTwoFlagContract[] =
    "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x50\x41"
    "\x89\xf7\x49\x89\xfe\x4c\x8b\x25\x00\x00\x00\x00\x49\x8b\x04\x24"
    "\x48\x89\x45\xd8\x48\x8d\x05\x00\x00\x00\x00\x8a\x00\x48\x8d\x0d"
    "\x00\x00\x00\x00\x0a\x01\x74\x25\x49\x8b\x04\x24\x48\x3b\x45\xd8"
    "\x0f\x85\x00\x00\x00\x00\x4c\x89\xf7\x44\x89\xfe\x48\x83\xc4\x50"
    "\x5b\x41\x5c\x41\x5e\x41\x5f\x5d\xe9\x00\x00\x00\x00\x49\x8b\x06"
    "\x4c\x89\xf7\xff\x50\x28\x44\x39\xf8\x0f\x8e\x00\x00\x00\x00\x31"
    "\xdb\x45\x85\xff\x41\x0f\x4f\xdf\x49\x8b\x7e\x28\x89\xde\xe8\x00"
    "\x00\x00\x00";
inline constexpr std::size_t kFmodDeviceListsSelectTwoFlagContractSize =
    sizeof(kFmodDeviceListsSelectTwoFlagContract) - 1;

inline constexpr std::size_t kFmodDeviceListsSelectContractMaxSize =
    std::max(kFmodDeviceListsSelectContractSize,
             kFmodDeviceListsSelectTwoFlagContractSize);

inline bool MatchesFmodSelectContract(
    std::string_view code, std::string_view contract,
    std::initializer_list<std::size_t> relocations) {
  if (code.size() < contract.size()) return false;
  for (std::size_t index = 0; index < contract.size(); ++index) {
    bool relocated = false;
    for (const std::size_t start : relocations) {
      if (index >= start && index < start + 4) relocated = true;
    }
    if (!relocated && code[index] != contract[index]) return false;
  }
  return true;
}

inline bool HasFmodDeviceListsSelectContract(std::string_view code) {
  return MatchesFmodSelectContract(
             code,
             {kFmodDeviceListsSelectContract,
              kFmodDeviceListsSelectContractSize},
             {24, 38, 55, 78, 96, 116}) ||
         MatchesFmodSelectContract(
             code,
             {kFmodDeviceListsSelectTwoFlagContract,
              kFmodDeviceListsSelectTwoFlagContractSize},
             {24, 39, 48, 66, 89, 107, 127});
}

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_FMOD_OUTPUT_DEVICE_CONTRACT_H_
