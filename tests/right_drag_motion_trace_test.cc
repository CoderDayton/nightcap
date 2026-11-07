#include "window/right_drag_motion_trace.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace mocktail::window {
namespace {

platform::MouseMotionEvent Absolute(float x, float y, float dx, float dy) {
  return {x, y, dx, dy, 0, false};
}

platform::MouseMotionEvent Relative(float dx, float dy) {
  return {0, 0, dx, dy, 4, true};
}

std::vector<std::string> Feed(RightDragMotionTrace& trace,
                              const std::vector<platform::MouseMotionEvent>& events) {
  std::vector<std::string> lines;
  for (const auto& event : events) {
    for (auto& line : trace.Observe(event)) lines.push_back(std::move(line));
  }
  return lines;
}

TEST(RightDragMotionTraceTest, SummarizesADragWhenThePointerIsFreeAgain) {
  RightDragMotionTrace trace;
  const auto lines = Feed(trace, {Absolute(100, 50, 1, 0), Relative(1, 1),
                                  Relative(4, -2), Relative(1, 0),
                                  Absolute(103, 50, 2, 0)});
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0],
            "  [cursor] drag 1: events=3 first=1,1 largest=4,-2@2 "
            "total=6,-1 release=+3,+0 next=2,0\n");
}

TEST(RightDragMotionTraceTest, LogsAJumpInsideADragAsItHappens) {
  RightDragMotionTrace trace;
  const auto lines =
      Feed(trace, {Absolute(10, 10, 0, 0), Relative(2, 0), Relative(-150, 3)});
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0],
            "  [cursor] drag 1 jump: delta=-150,3 at #2 relative=1 buttons=4\n");
}

TEST(RightDragMotionTraceTest, LogsAJumpRightAfterTheDragEnds) {
  RightDragMotionTrace trace;
  const auto lines =
      Feed(trace, {Absolute(10, 10, 0, 0), Relative(2, 0),
                   Absolute(10, 10, 0, 0), Absolute(400, 10, 390, 0),
                   Absolute(401, 10, 1, 0), Absolute(402, 10, 1, 0),
                   Absolute(900, 10, 498, 0)});
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1],
            "  [cursor] drag 1 jump: delta=390,0 at release+2 relative=0 "
            "buttons=0\n");
}

TEST(RightDragMotionTraceTest, StaysQuietWithoutDrags) {
  RightDragMotionTrace trace;
  EXPECT_TRUE(Feed(trace, {Absolute(1, 1, 1, 1), Absolute(500, 1, 499, 0)})
                  .empty());
}

TEST(RightDragMotionTraceTest, StopsAfterItsDragLimit) {
  RightDragMotionTrace trace;
  std::vector<std::string> lines;
  for (std::size_t drag = 0; drag < RightDragMotionTrace::kMaxDrags + 5;
       ++drag) {
    for (auto& line : Feed(trace, {Relative(1, 0), Absolute(1, 1, 0, 0)})) {
      lines.push_back(std::move(line));
    }
  }
  ASSERT_EQ(lines.size(), RightDragMotionTrace::kMaxDrags + 1);
  EXPECT_EQ(lines.back(), "  [cursor] drag logging stopped after 300 drags\n");
}

}  // namespace
}  // namespace mocktail::window
