#include "mocktail/graphics/angle_probe.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

#include <initializer_list>
#include <string>
#include <utility>

namespace mocktail {
namespace graphics {
namespace {

class SharedLibrary final {
 public:
  explicit SharedLibrary(const std::string& path)
      : handle_(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {}

  ~SharedLibrary() {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }

  SharedLibrary(const SharedLibrary&) = delete;
  SharedLibrary& operator=(const SharedLibrary&) = delete;

  bool valid() const { return handle_ != nullptr; }
  void* Find(const char* name) const {
    return handle_ != nullptr ? dlsym(handle_, name) : nullptr;
  }

 private:
  void* handle_ = nullptr;
};

BackendCapability Unavailable(std::string detail) {
  return {GraphicsBackendKind::kAngleVulkan, CapabilityState::kUnavailable,
          HardwareAcceleration::kUnknown, std::move(detail)};
}

std::string DlErrorFor(const char* library_kind, const std::string& path) {
  std::string detail = "failed to load ";
  detail += library_kind;
  detail += " library '";
  detail += path;
  detail += "': ";
  const char* error = dlerror();
  detail += error != nullptr ? error : "unknown dynamic loader error";
  return detail;
}

bool HasSymbols(const SharedLibrary& library,
                const std::initializer_list<const char*>& symbols,
                std::string* missing_symbol) {
  for (const char* symbol : symbols) {
    if (library.Find(symbol) == nullptr) {
      if (missing_symbol != nullptr) {
        *missing_symbol = symbol;
      }
      return false;
    }
  }
  return true;
}

}  // namespace

BackendCapability InspectAngleLibraries(const AngleProbeOptions& options) {
  if (options.egl_library_path.empty() || options.gles_library_path.empty()) {
    return Unavailable(
        "pinned ANGLE EGL and GLES library paths are both required");
  }

  dlerror();
  SharedLibrary egl(options.egl_library_path);
  if (!egl.valid()) {
    return Unavailable(DlErrorFor("EGL", options.egl_library_path));
  }
  dlerror();
  SharedLibrary gles(options.gles_library_path);
  if (!gles.valid()) {
    return Unavailable(DlErrorFor("GLES", options.gles_library_path));
  }

  std::string missing;
  if (!HasSymbols(
          egl,
          {"eglGetProcAddress", "eglInitialize", "eglTerminate",
           "eglQueryString", "eglChooseConfig", "eglCreateContext",
           "eglCreateWindowSurface", "eglMakeCurrent", "eglSwapBuffers"},
          &missing)) {
    return Unavailable("ANGLE EGL library is missing required symbol " +
                       missing);
  }
  if (!HasSymbols(
          gles,
          {"glGetString", "glGetIntegerv", "glCreateShader", "glDrawElements"},
          &missing)) {
    return Unavailable("ANGLE GLES library is missing required symbol " +
                       missing);
  }

  auto query_string =
      reinterpret_cast<PFNEGLQUERYSTRINGPROC>(egl.Find("eglQueryString"));
  const char* extensions = query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  const std::string extension_list =
      std::string(" ") + (extensions != nullptr ? extensions : "") + " ";
  if (extension_list.find(" EGL_ANGLE_platform_angle ") == std::string::npos) {
    return Unavailable("EGL library does not support EGL_ANGLE_platform_angle");
  }
  return {GraphicsBackendKind::kAngleVulkan, CapabilityState::kLoadable,
          HardwareAcceleration::kUnknown,
          "ANGLE EGL/GLES entry points available"};
}

}  // namespace graphics
}  // namespace mocktail
