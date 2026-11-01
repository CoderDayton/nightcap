#include "update/background_update_process.h"

#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <csignal>

namespace mocktail::update {

bool EnterBackgroundUpdateMode(pid_t parent) {
  // Without an explicit I/O priority the kernel derives one from the nice
  // value, so this also puts disk reads behind the game's.
  (void)setpriority(PRIO_PROCESS, 0, 19);
  if (parent <= 0) return true;
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) return false;
  // The parent may have exited before the death signal was armed.
  return getppid() == parent;
}

}  // namespace mocktail::update
