#include "runtime/display_size.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace runtime {
namespace {

void ExpectSize(const DisplaySize& size, int width, int height) {
  EXPECT_EQ(size.width, width);
  EXPECT_EQ(size.height, height);
}

TEST(DisplaySizeTest, ParsesHostDisplayPixels) {
  ExpectSize(ParseDisplaySize("3440x1440"), 3440, 1440);
  ExpectSize(ParseDisplaySize("1280x720"), 1280, 720);
}

TEST(DisplaySizeTest, FallsBackForMissingOrMalformedValues) {
  ExpectSize(ParseDisplaySize(nullptr), 1920, 1080);
  ExpectSize(ParseDisplaySize(""), 1920, 1080);
  ExpectSize(ParseDisplaySize("3440"), 1920, 1080);
  ExpectSize(ParseDisplaySize("3440x"), 1920, 1080);
  ExpectSize(ParseDisplaySize("x1440"), 1920, 1080);
  ExpectSize(ParseDisplaySize("3440x1440junk"), 1920, 1080);
  ExpectSize(ParseDisplaySize("-3440x1440"), 1920, 1080);
  ExpectSize(ParseDisplaySize("0x1440"), 1920, 1080);
}

TEST(DisplaySizeTest, FallsBackWhenAValueDoesNotFitAJavaInt) {
  ExpectSize(ParseDisplaySize("2147483648x1440"), 1920, 1080);
  ExpectSize(ParseDisplaySize("99999999999999999999x1440"), 1920, 1080);
}

TEST(DisplaySizeTest, ConvertsPixelsToMillimetersAt160Dpi) {
  EXPECT_EQ(PixelsToMillimetersAt160Dpi(1280), 203);
  EXPECT_EQ(PixelsToMillimetersAt160Dpi(720), 114);
  EXPECT_EQ(PixelsToMillimetersAt160Dpi(3440), 546);
  EXPECT_EQ(PixelsToMillimetersAt160Dpi(0), 0);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
