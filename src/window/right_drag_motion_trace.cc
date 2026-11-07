#include "window/right_drag_motion_trace.h"

#include <cmath>
#include <cstdio>

namespace mocktail::window {

namespace {

constexpr float kJumpPixels = 100.0f;
constexpr std::size_t kEventsWatchedAfterRelease = 3;

long Round(float value) { return std::lround(value); }

bool IsJump(float dx, float dy) {
  return std::fabs(dx) >= kJumpPixels || std::fabs(dy) >= kJumpPixels;
}

}  // namespace

std::vector<std::string> RightDragMotionTrace::Observe(
    const platform::MouseMotionEvent& event) {
  std::vector<std::string> lines;
  if (stopped_) return lines;
  char line[192];
  const float dx = event.delta_x;
  const float dy = event.delta_y;

  if (event.relative) {
    if (!dragging_) {
      if (drags_ == kMaxDrags) {
        stopped_ = true;
        std::snprintf(line, sizeof(line),
                      "  [cursor] drag logging stopped after %zu drags\n",
                      kMaxDrags);
        lines.emplace_back(line);
        return lines;
      }
      dragging_ = true;
      ++drags_;
      events_ = 0;
      first_x_ = dx;
      first_y_ = dy;
      largest_x_ = largest_y_ = 0;
      largest_at_ = 0;
      total_x_ = total_y_ = 0;
      has_start_ = has_position_;
      start_x_ = position_x_;
      start_y_ = position_y_;
    }
    ++events_;
    total_x_ += dx;
    total_y_ += dy;
    if (std::hypot(dx, dy) > std::hypot(largest_x_, largest_y_)) {
      largest_x_ = dx;
      largest_y_ = dy;
      largest_at_ = events_;
    }
    if (IsJump(dx, dy)) {
      std::snprintf(line, sizeof(line),
                    "  [cursor] drag %zu jump: delta=%ld,%ld at #%zu "
                    "relative=1 buttons=%u\n",
                    drags_, Round(dx), Round(dy), events_, event.buttons);
      lines.emplace_back(line);
    }
    return lines;
  }

  has_position_ = true;
  position_x_ = event.x;
  position_y_ = event.y;
  if (dragging_) {
    dragging_ = false;
    after_release_ = 0;
    char release[48] = "";
    if (has_start_) {
      std::snprintf(release, sizeof(release), " release=%+ld,%+ld",
                    Round(event.x - start_x_), Round(event.y - start_y_));
    }
    std::snprintf(line, sizeof(line),
                  "  [cursor] drag %zu: events=%zu first=%ld,%ld "
                  "largest=%ld,%ld@%zu total=%ld,%ld%s next=%ld,%ld\n",
                  drags_, events_, Round(first_x_), Round(first_y_),
                  Round(largest_x_), Round(largest_y_), largest_at_,
                  Round(total_x_), Round(total_y_), release, Round(dx),
                  Round(dy));
    lines.emplace_back(line);
  }
  if (drags_ == 0 || after_release_ >= kEventsWatchedAfterRelease) {
    return lines;
  }
  ++after_release_;
  if (IsJump(dx, dy)) {
    std::snprintf(line, sizeof(line),
                  "  [cursor] drag %zu jump: delta=%ld,%ld at release+%zu "
                  "relative=0 buttons=%u\n",
                  drags_, Round(dx), Round(dy), after_release_, event.buttons);
    lines.emplace_back(line);
  }
  return lines;
}

}  // namespace mocktail::window
