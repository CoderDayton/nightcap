#include "mocktail/vr/openxr_backend.h"
#include "mocktail/vr/roblox_vr_device_bridge.h"
#include "vr/gles_transport.h"
#include <GLES3/gl3.h>
#include <SDL3/SDL.h>
#include <array>
#include <cstdlib>
#include <gtest/gtest.h>
#include <openxr/openxr.h>

namespace mocktail::vr {
namespace internal {
struct VrBridgeTestAccess {
  static void Enable(RobloxVrDeviceBridge &bridge) { bridge.active_ = true; }
};
} // namespace internal
struct GlesTransportTestAccess {
  static void Owner(GlesTransport &t, unsigned fb, void *owner) {
    t.owners_[fb] = owner;
  }
};
struct OpenXrBackendTestAccess {
  static GlesTransport &Transport(OpenXrBackend &b) { return *b.gles_; }
  static void LoseSession(OpenXrBackend &b) {
    b.HandleSessionState(XR_SESSION_STATE_LOSS_PENDING);
  }
};
namespace {
#define GL(name)                                                               \
  reinterpret_cast<decltype(&gl##name)>(SDL_GL_GetProcAddress("gl" #name))
class GlesVrTransportTest : public ::testing::Test {
protected:
  void SetUp() override {
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    window = SDL_CreateWindow("Mocktail GLES VR test", 64, 64,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    ASSERT_NE(window, nullptr) << SDL_GetError();
    context = SDL_GL_CreateContext(window);
    ASSERT_NE(context, nullptr) << SDL_GetError();
    ASSERT_TRUE(SDL_GL_MakeCurrent(window, context));
    internal::VrBridgeTestAccess::Enable(bridge);
  }
  void TearDown() override {
    transport.Destroy();
    if (context)
      SDL_GL_DestroyContext(context);
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
  }
  void InitTransport(GlesTransport &t) {
    std::string error;
    ASSERT_TRUE(t.Initialize(
        SDL_EGL_GetCurrentDisplay(), SDL_EGL_GetCurrentConfig(), context,
        reinterpret_cast<void *>(SDL_EGL_GetProcAddress("eglGetProcAddress")),
        Resolve, &error))
        << error;
  }
  static void *Resolve(const char *name) {
    return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
  }
  GLuint Texture(GlesTransport *t, float r, float g, float b) {
    GLuint texture = 0, fb = 0;
    GL(GenTextures)(1, &texture);
    GL(BindTexture)(GL_TEXTURE_2D, texture);
    GL(TexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, 500, 500);
    if (t)
      t->Storage(true, 500, 500, GL_RGBA8);
    GL(GenFramebuffers)(1, &fb);
    GL(BindFramebuffer)(GL_FRAMEBUFFER, fb);
    GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, texture, 0);
    GL(ClearColor)(r, g, b, 1);
    GL(Clear)(GL_COLOR_BUFFER_BIT);
    GL(DeleteFramebuffers)(1, &fb);
    return texture;
  }
  GLuint Eye(GlesTransport &t, int eye, GLuint texture, std::uint64_t frame = 7,
             void *owner = reinterpret_cast<void *>(123)) {
    GLuint fb = 0;
    GL(GenFramebuffers)(1, &fb);
    GL(BindFramebuffer)(GL_FRAMEBUFFER, fb);
    GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, texture, 0);
    GlesTransportTestAccess::Owner(t, fb, owner);
    BindEye(t, eye, fb, frame, owner);
    return fb;
  }
  void BindEye(GlesTransport &t, int eye, GLuint fb, std::uint64_t frame,
               void *owner) {
    GL(BindFramebuffer)(GL_FRAMEBUFFER, fb);
    bridge.OnEyeGetterCall(owner, eye,
                           reinterpret_cast<void *>(std::uintptr_t(fb)));
    t.Bind(GL_FRAMEBUFFER, fb, frame);
  }
  std::array<unsigned char, 4> Pixel(GLuint texture, int x = 250, int y = 250) {
    GLuint fb = 0;
    GL(GenFramebuffers)(1, &fb);
    GL(BindFramebuffer)(GL_FRAMEBUFFER, fb);
    GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, texture, 0);
    std::array<unsigned char, 4> rgba{};
    GL(ReadPixels)(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    GL(DeleteFramebuffers)(1, &fb);
    return rgba;
  }
  SDL_Window *window = nullptr;
  SDL_GLContext context = nullptr;
  GlesTransport transport;
  RobloxVrDeviceBridge bridge;
};
TEST_F(GlesVrTransportTest, CopiesDistinctEyesAndRestoresGuestState) {
  InitTransport(transport);
  const GLuint red = Texture(&transport, 1, 0, 0),
               green = Texture(&transport, 0, 1, 0);
  Eye(transport, 0, red);
  Eye(transport, 1, green);
  const GLuint targets[2] = {Texture(nullptr, 0, 0, 0),
                             Texture(nullptr, 0, 0, 0)};
  // A tiny guest scissor must not crop the eye copy, and bindings must survive.
  GLuint fbs[2];
  GL(GenFramebuffers)(2, fbs);
  GL(BindFramebuffer)(GL_READ_FRAMEBUFFER, fbs[0]);
  GL(BindFramebuffer)(GL_DRAW_FRAMEBUFFER, fbs[1]);
  GL(Enable)(GL_SCISSOR_TEST);
  GL(Scissor)(0, 0, 1, 1);
  GL(Viewport)(2, 3, 17, 19);
  ASSERT_TRUE(transport.Copy(targets, 500, 500));
  GLint read = 0, draw = 0, viewport[4];
  GL(GetIntegerv)(GL_READ_FRAMEBUFFER_BINDING, &read);
  GL(GetIntegerv)(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  GL(GetIntegerv)(GL_VIEWPORT, viewport);
  EXPECT_EQ(static_cast<GLuint>(read), fbs[0]);
  EXPECT_EQ(static_cast<GLuint>(draw), fbs[1]);
  EXPECT_TRUE(GL(IsEnabled)(GL_SCISSOR_TEST));
  EXPECT_EQ(viewport[0], 2);
  EXPECT_EQ(viewport[1], 3);
  EXPECT_EQ(viewport[2], 17);
  EXPECT_EQ(viewport[3], 19);
  EXPECT_EQ(Pixel(targets[0]), (std::array<unsigned char, 4>{255, 0, 0, 255}));
  EXPECT_EQ(Pixel(targets[1]), (std::array<unsigned char, 4>{0, 255, 0, 255}));
  EXPECT_EQ(GL(GetError)(), GL_NO_ERROR);
}
TEST_F(GlesVrTransportTest, RejectsUnownedAndAliasedEyesAndTracksDeletion) {
  InitTransport(transport);
  GLuint texture = Texture(&transport, 1, 0, 0);
  GLuint fbs[2];
  GL(GenFramebuffers)(2, fbs);
  GL(BindFramebuffer)(GL_FRAMEBUFFER, fbs[0]);
  GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
  BindEye(transport, 0, fbs[0], 7, reinterpret_cast<void *>(123));
  EXPECT_EQ(transport.eye(0).framebuffer, 0u);
  Eye(transport, 0, texture);
  Eye(transport, 1, texture);
  const GLuint targets[2] = {Texture(nullptr, 0, 0, 0),
                             Texture(nullptr, 0, 0, 0)};
  EXPECT_FALSE(transport.Copy(targets, 500, 500));
  transport.ObjectsDeleted(true, 1, &texture);
  EXPECT_EQ(transport.eye(0).framebuffer, 0u);
  EXPECT_EQ(transport.eye(1).framebuffer, 0u);
}
TEST_F(GlesVrTransportTest, NewOwnerAndFramebufferDeletionInvalidateOldPair) {
  InitTransport(transport);
  const GLuint red = Texture(&transport, 1, 0, 0),
               green = Texture(&transport, 0, 1, 0);
  Eye(transport, 0, red);
  Eye(transport, 1, green);
  const auto fb = Eye(transport, 0, red, 8, reinterpret_cast<void *>(456));
  EXPECT_EQ(transport.eye(1).framebuffer, 0u);
  EXPECT_EQ(transport.eye(0).frame, 8u);
  transport.Deleted(1, &fb);
  EXPECT_EQ(transport.eye(0).framebuffer, 0u);
}
TEST_F(GlesVrTransportTest, RenderbufferEyesAndAttachmentMutation) {
  InitTransport(transport);
  GLuint rb = 0, fb = 0;
  GL(GenRenderbuffers)(1, &rb);
  GL(BindRenderbuffer)(GL_RENDERBUFFER, rb);
  GL(RenderbufferStorage)(GL_RENDERBUFFER, GL_RGBA8, 500, 500);
  transport.Storage(false, 500, 500, GL_RGBA8);
  GL(GenFramebuffers)(1, &fb);
  GL(BindFramebuffer)(GL_FRAMEBUFFER, fb);
  GL(FramebufferRenderbuffer)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb);
  GL(ClearColor)(0, 0, 1, 1);
  GL(Clear)(GL_COLOR_BUFFER_BIT);
  GlesTransportTestAccess::Owner(transport, fb, reinterpret_cast<void *>(123));
  BindEye(transport, 0, fb, 7, reinterpret_cast<void *>(123));
  EXPECT_EQ(transport.eye(0).object, rb);
  EXPECT_FALSE(transport.eye(0).texture);
  const GLuint green = Texture(&transport, 0, 1, 0);
  Eye(transport, 1, green);
  const GLuint targets[2] = {Texture(nullptr, 0, 0, 0),
                             Texture(nullptr, 0, 0, 0)};
  ASSERT_TRUE(transport.Copy(targets, 500, 500));
  EXPECT_EQ(Pixel(targets[0]), (std::array<unsigned char, 4>{0, 0, 255, 255}));
  GL(BindFramebuffer)(GL_FRAMEBUFFER, fb);
  transport.AttachmentChanged(GL_FRAMEBUFFER);
  EXPECT_EQ(transport.eye(0).framebuffer, 0u);
  EXPECT_EQ(GL(GetError)(), GL_NO_ERROR);
}
TEST_F(GlesVrTransportTest, OpenXrEglSessionSubmitsAndRecovers) {
  if (!std::getenv("MOCKTAIL_TEST_XR_GLES"))
    GTEST_SKIP() << "Opt-in: requires a connected or simulated OpenXR runtime";
  OpenXrBackend backend;
  auto status = backend.Arm(VrGraphicsApi::kOpenGles);
  ASSERT_TRUE(status.ok()) << status.message();
  status = backend.AttachGlesContext(
      SDL_EGL_GetCurrentDisplay(), SDL_EGL_GetCurrentConfig(), context,
      reinterpret_cast<void *>(SDL_EGL_GetProcAddress("eglGetProcAddress")),
      Resolve);
  ASSERT_TRUE(status.ok()) << status.message();
  auto &t = OpenXrBackendTestAccess::Transport(backend);
  const GLuint red = Texture(&t, 1, 0, 0), green = Texture(&t, 0, 1, 0);
  auto *owner = reinterpret_cast<void *>(123);
  const GLuint fbs[2] = {Eye(t, 0, red), Eye(t, 1, green)};
  bool lost_session = false;
  for (int i = 0; i < 200 && backend.submitted_frames() < 20; ++i) {
    if (!lost_session && backend.submitted_frames() >= 10) {
      OpenXrBackendTestAccess::LoseSession(backend);
      EXPECT_FALSE(backend.session_running());
      EXPECT_FALSE(backend.PublishedHeadPose().valid);
      lost_session = true;
    }
    backend.NoteGlesPresent();
    auto pose = backend.PublishedHeadPose();
    if (!pose.valid) {
      SDL_Delay(10);
      continue;
    }
    backend.NotePoseApplied(owner, pose.frame);
    BindEye(t, 0, fbs[0], pose.frame, owner);
    BindEye(t, 1, fbs[1], pose.frame, owner);
    EXPECT_EQ(t.eye(0).frame, pose.frame);
    EXPECT_EQ(t.eye(1).frame, pose.frame);
    EXPECT_TRUE(t.IsCurrent());
  }
  EXPECT_TRUE(lost_session);
  EXPECT_GE(backend.submitted_frames(), 20u);
  EXPECT_EQ(GL(GetError)(), GL_NO_ERROR);
  backend.DetachGlesContext(context);
  EXPECT_FALSE(backend.session_running());
  status = backend.AttachGlesContext(
      SDL_EGL_GetCurrentDisplay(), SDL_EGL_GetCurrentConfig(), context,
      reinterpret_cast<void *>(SDL_EGL_GetProcAddress("eglGetProcAddress")),
      Resolve);
  EXPECT_TRUE(status.ok()) << status.message();
  if (status.ok()) {
    EXPECT_EQ(OpenXrBackendTestAccess::Transport(backend).eye(0).framebuffer,
              0u);
    backend.DetachGlesContext(context);
  }
}
} // namespace
} // namespace mocktail::vr
