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

// Unthrottled presentation must not pace the main-thread pump.
//
// Roblox posts its per-frame main-thread step at a time of its own choosing
// and nothing signals the host, so the pump is a poll. A frame that misses
// its slot slips a whole period, and the engine's slack is under a
// millisecond, so the pacer's 240 Hz cadence is far too coarse here. The rest
// between polls is runtime/engine_pump_rest, bounded at 100 us.
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
