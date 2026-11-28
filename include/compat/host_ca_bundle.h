#ifndef MOCKTAIL_COMPAT_HOST_CA_BUNDLE_H_
#define MOCKTAIL_COMPAT_HOST_CA_BUNDLE_H_

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <string>

namespace mocktail::compat {

inline constexpr std::array<const char*, 2> kDefaultHostCaBundles = {
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
};

// Returns the resolved path of a readable, non-empty regular file, or "".
// The resolved path matters on FreeBSD's Linuxulator: it does not re-root an
// absolute symlink target into /compat/linux, so opening Fedora's
// ca-certificates.crt symlink fails while the path realpath() returns opens.
inline std::string CanonicalCaBundlePath(const std::string& path) {
  if (path.empty() || path.front() != '/') return {};
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) == nullptr || resolved[0] != '/') {
    return {};
  }
  const int fd = ::open(resolved, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  struct stat status {};
  const bool usable = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
                      status.st_size > 0 &&
                      (status.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0;
  const bool closed = ::close(fd) == 0;
  return usable && closed ? std::string(resolved) : std::string();
}

template <typename Candidates>
std::string FindHostCaBundle(const Candidates& candidates) {
  for (const auto& candidate : candidates) {
    std::string resolved = CanonicalCaBundlePath(candidate);
    if (!resolved.empty()) return resolved;
  }
  return {};
}

inline std::string FindHostCaBundle() {
  return FindHostCaBundle(kDefaultHostCaBundles);
}

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_HOST_CA_BUNDLE_H_
