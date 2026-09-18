// Drives the real PaceInputPump() against an SDL window, so the pacing
// decision is exercised end to end rather than only as pacer arithmetic.
// Gated like the other tests that need a real SDL video driver.
#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "window/input_pump_pacer.h"
#include "window/window.h"

namespace mocktail {
namespace window {
namespace {

bool RealSdlWindowTestsEnabled() {
  const char* enabled = std::getenv("MOCKTAIL_TEST_SDL_WINDOW");
  return enabled != nullptr && std::string(enabled) == "1";
}

// Unthrottled presentation must not park the main-thread pump.
//
// Roblox's render thread posts work to the main thread and waits on the
// reply. That queue is inside libroblox.so, so polling is the only way to
// observe it and a timed sleep cannot be woken by a post. A frame that misses
// its slot slips a whole period, and the engine's slack before that happens
// is under a millisecond, shorter than any sleep long enough to save CPU.
//
// A throttled present blocks on vsync, so the pacer still runs there.
TEST(InputPumpPacingRuntimeTest, UnthrottledPresentationDoesNotParkThePump) {
  if (!RealSdlWindowTestsEnabled()) {
    GTEST_SKIP() << "real SDL window test was not explicitly enabled";
  }
  ASSERT_EQ(setenv("MOCKTAIL_VSYNC", "off", 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS", "1", 1), 0);
  ASSERT_TRUE(Init(320, 180, "input pump pacing test"));
  ASSERT_TRUE(UnthrottledPresentationRequested());

  constexpr int kTicks = 240;
  uint64_t slept_ns = 0;
  for (int tick = 0; tick < kTicks; ++tick) {
    PumpEvents();
    slept_ns += PaceInputPump();
  }
  Shutdown();

  EXPECT_EQ(slept_ns, 0U)
      << "unthrottled ticks parked the main-thread pump for " << slept_ns
      << " ns, which delays the engine's main-thread replies";
}

// The present mode policy is cached process-wide on first use, so a second
// Init in this binary reports the mode selected above. The throttled cadence
// is covered by the pacer's own arithmetic tests.

}  // namespace
}  // namespace window
}  // namespace mocktail
