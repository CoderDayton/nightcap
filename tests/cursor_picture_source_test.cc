#include "services/cursor_picture_source.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using mocktail::services::CursorAssetId;
using mocktail::services::CursorPictureSource;
using mocktail::services::HttpClient;
using mocktail::services::HttpRequest;
using mocktail::services::HttpResponse;
using mocktail::services::LocalCursorAssetPath;
using mocktail::services::LocalCursorPictureCandidates;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_cursor_picture_source_XXXXXX";
    if (char* created = mkdtemp(pattern); created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

class FakeHttpClient final : public HttpClient {
 public:
  HttpResponse Get(const HttpRequest& request) override {
    requests.push_back(request);
    if (responses.empty()) {
      return {};
    }
    HttpResponse response = responses.front();
    responses.erase(responses.begin());
    return response;
  }

  std::vector<HttpRequest> requests;
  std::vector<HttpResponse> responses;
};

bool HasCookie(const HttpRequest& request) {
  for (const std::string& header : request.headers) {
    if (header.find("ROBLOSECURITY") != std::string::npos) {
      return true;
    }
  }
  return false;
}

TEST(CursorPictureSourceTest, NamesLocalCursorFiles) {
  EXPECT_EQ(LocalCursorAssetPath("rbxasset://textures//DragCursor.png"),
            "textures/DragCursor.png");
  EXPECT_EQ(LocalCursorAssetPath(
                "!bxasset://textures/Cursors/KeyboardMouse/IBeamCursor.png"),
            "textures/Cursors/KeyboardMouse/IBeamCursor.png");
  EXPECT_EQ(LocalCursorAssetPath("rbxasset://../secret.png"), "");
  EXPECT_EQ(LocalCursorAssetPath("rbxasset://textures/../../x.png"), "");
  EXPECT_EQ(LocalCursorAssetPath("rbxassetid://123"), "");
}

TEST(CursorPictureSourceTest, ReadsAssetIds) {
  EXPECT_EQ(CursorAssetId("rbxassetid://1234567"), 1234567U);
  EXPECT_EQ(CursorAssetId("http://www.roblox.com/asset/?id=42"), 42U);
  EXPECT_EQ(CursorAssetId("https://www.roblox.com/asset?version=1&id=42"),
            42U);
  EXPECT_EQ(CursorAssetId("https://assetdelivery.roblox.com/v1/asset/?id=7"),
            7U);
  EXPECT_FALSE(CursorAssetId("https://example.com/asset/?id=42").has_value());
  EXPECT_FALSE(CursorAssetId("rbxassetid://").has_value());
  EXPECT_FALSE(
      CursorAssetId("rbxasset://textures/ArrowCursor.png").has_value());
}

TEST(CursorPictureSourceTest, LooksInTheOverlayFirst) {
  const std::vector<std::filesystem::path> candidates =
      LocalCursorPictureCandidates("textures/A.png", "/overlay", "/assets");
  ASSERT_EQ(candidates.size(), 4U);
  EXPECT_EQ(candidates[0], "/overlay/content/textures/A.png");
  EXPECT_EQ(candidates[2], "/assets/content/textures/A.png");
}

TEST(CursorPictureSourceTest, ReadsALocalCursorWithoutTheNetwork) {
  TemporaryDirectory directory;
  const std::filesystem::path overlay = directory.path() / "overlay";
  const std::filesystem::path assets = directory.path() / "assets";
  WriteFile(assets / "content/textures/Cursors/KeyboardMouse/ArrowCursor.png",
            "stock");
  WriteFile(overlay / "content/textures/IBeamCursor.png", "overlay");
  FakeHttpClient http;
  CursorPictureSource source(&http, directory.path() / "cookie", overlay,
                             [&] { return assets; });

  EXPECT_EQ(source.Fetch(
                "rbxasset://textures/Cursors/KeyboardMouse/ArrowCursor.png"),
            "stock");
  EXPECT_EQ(source.Fetch("rbxasset://textures/IBeamCursor.png"), "overlay");
  EXPECT_EQ(source.Fetch("rbxasset://textures/DragCursor.png"), "");
  EXPECT_TRUE(http.requests.empty());
}

TEST(CursorPictureSourceTest, SendsTheCookieOnlyToTheAssetService) {
  TemporaryDirectory directory;
  WriteFile(directory.path() / "cookie", ".ROBLOSECURITY=secret\n");
  FakeHttpClient http;
  http.responses.push_back(HttpResponse(
      true, 302, "", "",
      {"HTTP/2 302", "location: https://c3.rbxcdn.com/abc"}));
  http.responses.push_back(HttpResponse(true, 200, "picture", ""));
  CursorPictureSource source(&http, directory.path() / "cookie", {}, {});

  EXPECT_EQ(source.Fetch("rbxassetid://99"), "picture");
  ASSERT_EQ(http.requests.size(), 2U);
  EXPECT_EQ(http.requests[0].url,
            "https://assetdelivery.roblox.com/v1/asset/?id=99");
  EXPECT_TRUE(HasCookie(http.requests[0]));
  EXPECT_FALSE(http.requests[0].follow_redirects);
  EXPECT_EQ(http.requests[1].url, "https://c3.rbxcdn.com/abc");
  EXPECT_FALSE(HasCookie(http.requests[1]));
}

TEST(CursorPictureSourceTest, RefusesARedirectAwayFromRoblox) {
  TemporaryDirectory directory;
  WriteFile(directory.path() / "cookie", ".ROBLOSECURITY=secret\n");
  FakeHttpClient http;
  http.responses.push_back(HttpResponse(
      true, 302, "", "", {"Location: https://evil.example/rbxcdn.com"}));
  CursorPictureSource source(&http, directory.path() / "cookie", {}, {});

  EXPECT_EQ(source.Fetch("rbxassetid://99"), "");
  EXPECT_EQ(http.requests.size(), 1U);
}

TEST(CursorPictureSourceTest, DownloadsWithoutACookieWhenNoneIsSaved) {
  TemporaryDirectory directory;
  WriteFile(directory.path() / "cookie", "garbage\r\nInjected: 1");
  FakeHttpClient http;
  http.responses.push_back(HttpResponse(true, 200, "picture", ""));
  CursorPictureSource source(&http, directory.path() / "cookie", {}, {});

  EXPECT_EQ(source.Fetch("rbxassetid://5"), "picture");
  ASSERT_EQ(http.requests.size(), 1U);
  EXPECT_TRUE(http.requests[0].headers.empty());
}

}  // namespace
