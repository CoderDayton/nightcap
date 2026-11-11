#ifndef MOCKTAIL_SCOPED_FILE_DESCRIPTOR_H_
#define MOCKTAIL_SCOPED_FILE_DESCRIPTOR_H_

#include <unistd.h>

namespace mocktail {

// Closes a file descriptor when it leaves scope. Negative values are ignored.
class ScopedFileDescriptor final {
 public:
  explicit ScopedFileDescriptor(int descriptor = -1)
      : descriptor_(descriptor) {}
  ~ScopedFileDescriptor() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  int get() const { return descriptor_; }
  int release() {
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
  }

 private:
  int descriptor_ = -1;
};

}  // namespace mocktail

#endif  // MOCKTAIL_SCOPED_FILE_DESCRIPTOR_H_
