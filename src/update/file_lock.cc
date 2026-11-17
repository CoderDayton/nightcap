#include "update/file_lock.h"

#include <sys/file.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

namespace mocktail::update {

bool LockExclusiveWithin(int descriptor, std::chrono::milliseconds wait,
                         std::string_view what, std::string* error) {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  bool announced = false;
  while (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EINTR) continue;
    if (errno != EWOULDBLOCK) {
      *error = "cannot lock " + std::string(what) + ": " + std::strerror(errno);
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      *error = "another Nightcap held the " + std::string(what) + " for " +
               std::to_string(wait.count() / 1000) + " s";
      return false;
    }
    if (!announced) {
      announced = true;
      std::fprintf(stderr,
                   "  [update] waiting for another Nightcap to release the "
                   "%.*s\n",
                   static_cast<int>(what.size()), what.data());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  return true;
}

}
