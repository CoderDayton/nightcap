#ifndef MOCKTAIL_WINDOW_HOST_SURFACE_COMMIT_GATE_H_
#define MOCKTAIL_WINDOW_HOST_SURFACE_COMMIT_GATE_H_

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace mocktail {
namespace window {

// Keeps main-thread wl_surface commits out of a host vkQueuePresentKHR call.
// With explicit sync a Wayland driver sends its sync points, buffer attach
// and commit as separate requests inside that call. A commit from another
// thread between them carries sync points without a buffer, which is a
// no_buffer protocol error that disconnects the client.
class HostSurfaceCommitGate final {
 public:
  // A host present can block for as long as the compositor withholds a
  // frame, so the main thread proceeds unguarded after this wait.
  static constexpr std::chrono::milliseconds kMainThreadWait{50};

  HostSurfaceCommitGate() = default;
  HostSurfaceCommitGate(const HostSurfaceCommitGate&) = delete;
  HostSurfaceCommitGate& operator=(const HostSurfaceCommitGate&) = delete;

  void BeginHostPresent() {
    if (present_thread_.load(std::memory_order_acquire) ==
        std::this_thread::get_id()) {
      // The previous present on this thread never reported its end.
      return;
    }
    mutex_.lock();
    present_thread_.store(std::this_thread::get_id(),
                          std::memory_order_release);
  }

  void EndHostPresent() {
    if (present_thread_.load(std::memory_order_acquire) !=
        std::this_thread::get_id()) {
      return;
    }
    present_thread_.store(std::thread::id(), std::memory_order_release);
    mutex_.unlock();
  }

  // Held around SDL calls that can commit the window surface.
  class MainThreadScope final {
   public:
    explicit MainThreadScope(
        HostSurfaceCommitGate& gate,
        std::chrono::nanoseconds wait = kMainThreadWait)
        : lock_(gate.mutex_, wait) {}
    MainThreadScope(const MainThreadScope&) = delete;
    MainThreadScope& operator=(const MainThreadScope&) = delete;

    bool owns_lock() const { return lock_.owns_lock(); }

   private:
    std::unique_lock<std::timed_mutex> lock_;
  };

 private:
  std::timed_mutex mutex_;
  std::atomic<std::thread::id> present_thread_{};
};

inline HostSurfaceCommitGate& ProcessHostSurfaceCommitGate() {
  static HostSurfaceCommitGate gate;
  return gate;
}

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_HOST_SURFACE_COMMIT_GATE_H_
