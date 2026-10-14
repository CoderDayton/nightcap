// Counts heap allocations made on the audio thread by FMOD AudioDevice.write.
#include "jnivm/jnivm.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

namespace {
thread_local bool g_counting = false;
thread_local std::size_t g_allocations = 0;

void* CountedAllocate(std::size_t size) noexcept {
  if (g_counting) {
    ++g_allocations;
  }
  return std::malloc(size == 0 ? 1 : size);
}
}  // namespace

void* operator new(std::size_t size) {
  if (void* memory = CountedAllocate(size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return CountedAllocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return CountedAllocate(size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {

struct WriteProbe {
  int writes = 0;
};

TEST(FmodAudioDeviceWrite, SteadyStateWriteDoesNotAllocate) {
  auto vm = std::make_shared<jnivm::VM>();
  JNIEnv* env = vm->GetJNIEnv();
  auto probe = std::make_shared<WriteProbe>();
  jnivm::FmodAudioDeviceCallbacks callbacks;
  callbacks.write = [](void* context, const void*, const std::uint8_t*,
                       std::size_t) {
    ++static_cast<WriteProbe*>(context)->writes;
    return true;
  };
  vm->SetFmodAudioDeviceCallbacks(probe, callbacks);

  jclass device_class = env->FindClass("org/fmod/AudioDevice");
  jobject device = env->NewObject(
      device_class, env->GetMethodID(device_class, "<init>", "()V"));
  const jmethodID write = env->GetMethodID(device_class, "write", "([BI)V");
  jbyteArray block = env->NewByteArray(4096);
  env->CallVoidMethod(device, write, block, 4096);

  g_allocations = 0;
  g_counting = true;
  for (int i = 0; i < 64; ++i) {
    env->CallVoidMethod(device, write, block, 4096);
  }
  g_counting = false;

  EXPECT_EQ(g_allocations, 0u);
  EXPECT_EQ(probe->writes, 65);
  vm->ClearFmodAudioDeviceCallbacks();
}

}  // namespace
