#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mocktail/platform/platform_runtime.h"

namespace mocktail::window {

// Session-log trace of each relative-mode camera drag: one summary line per
// drag, plus a line for any single motion of 100 px or more during the drag or
// in the first events after it. Costs a few comparisons per motion event.
class RightDragMotionTrace {
 public:
  static constexpr std::size_t kMaxDrags = 300;

  std::vector<std::string> Observe(const platform::MouseMotionEvent& event);

 private:
  bool dragging_ = false;
  bool stopped_ = false;
  std::size_t drags_ = 0;
  std::size_t events_ = 0;
  std::size_t after_release_ = 0;
  float first_x_ = 0, first_y_ = 0;
  float largest_x_ = 0, largest_y_ = 0;
  std::size_t largest_at_ = 0;
  float total_x_ = 0, total_y_ = 0;
  bool has_position_ = false;
  float position_x_ = 0, position_y_ = 0;
  bool has_start_ = false;
  float start_x_ = 0, start_y_ = 0;
};

}  // namespace mocktail::window
