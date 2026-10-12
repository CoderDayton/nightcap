#ifndef MOCKTAIL_VR_GLES_TRANSPORT_H_
#define MOCKTAIL_VR_GLES_TRANSPORT_H_

#include <cstdint>
#include <string>
#include <unordered_map>

namespace mocktail::vr {
// One guest EGL context. GL callbacks and copies run on its current thread.
// Only framebuffers created inside the verified DebugDeviceVR initializer
// can become eyes; a live eye-getter request supplies the eye index.
class GlesTransport {
public:
  struct Eye {
    unsigned framebuffer = 0;
    unsigned object = 0;
    bool texture = false;
    int width = 0, height = 0;
    void *owner = nullptr;
    std::uint64_t frame = 0;
  };
  bool Initialize(void *display, void *config, void *context,
                  void *egl_get_proc, void *(*resolve)(const char *),
                  std::string *error);
  bool IsCurrent() const;
  void Destroy(); // Requires this context current; call before EGL destruction.
  void MakeCurrent();
  static void ReleaseCurrent();
  static void *Wrap(const char *name, void *raw);
  void ResetEyes();
  bool Copy(const unsigned textures[2], unsigned width, unsigned height);
  const Eye &eye(int index) const { return eyes_[index]; }
  void *display() const { return display_; }
  void *config() const { return config_; }
  void *context() const { return context_; }
  void *egl_get_proc() const { return egl_get_proc_; }
  std::uint64_t version() const { return version_; }

  // Called after the real GL operation, never while the backend mutex is held.
  void Generated(int count, const unsigned *ids);
  void Deleted(int count, const unsigned *ids);
  void Storage(bool texture, int width, int height, unsigned format,
               int samples = 0);
  void ObjectsDeleted(bool texture, int count, const unsigned *ids);
  void AttachmentChanged(unsigned target);
  void Bind(unsigned target, unsigned framebuffer,
            std::uint64_t frame = UINT64_MAX);

private:
  friend struct GlesTransportTestAccess;
  struct StorageInfo {
    int width = 0, height = 0;
    unsigned format = 0;
    int samples = 0;
  };
  void *display_ = nullptr;
  void *config_ = nullptr;
  void *context_ = nullptr;
  void *egl_get_proc_ = nullptr;
  std::uint64_t version_ = 0;
  void *(*get_current_context_)() = nullptr;
  unsigned copy_fbos_[2] = {};
  std::unordered_map<unsigned, void *> owners_;
  std::unordered_map<unsigned, StorageInfo> textures_;
  std::unordered_map<unsigned, StorageInfo> renderbuffers_;
  Eye eyes_[2];
};
} // namespace mocktail::vr
#endif
