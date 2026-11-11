// Manual benchmark: replays a Roblox-shaped JNI session through jnivm and
// reports wall time, CPU time, C++ heap allocations and RSS per phase.
// The call mix is modeled on the jnivm handlers, not a recorded trace.
//
//   jnivm_session_benchmark [--profile menu|ingame] [--fps N] [--minutes N]
//                           [--frames N] [--repeat N] [--json PATH]
//
// ingame: 120 FPS for an hour of a busy multiplayer game, with chat and
// message-bus rates assumed from the user's own sessions.
#include "jnivm/jnivm.h"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_allocated_bytes{0};

void* CountedAllocate(std::size_t size) noexcept {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  g_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
  return std::malloc(size == 0 ? 1 : size);
}
}  // namespace

void* operator new(std::size_t size) {
  if (void* memory = CountedAllocate(size)) return memory;
  std::abort();
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

constexpr int kAudioWritesPerSecond = 47;  // 48 kHz, 1024-frame blocks.
constexpr std::size_t kAudioBlockBytes = 4096;

// Per-second rates, converted to frame intervals at the chosen FPS.
struct Workload {
  const char* name = "menu";
  int fps = 60;
  int input_events_per_frame = 3;
  int bus_messages_per_second = 15;
  int chat_updates_per_second = 2;
  int telemetry_batches_per_second = 1;
};

Workload MenuWorkload() { return {}; }

Workload InGameWorkload() {
  Workload workload;
  workload.name = "ingame";
  workload.fps = 120;
  workload.input_events_per_frame = 4;
  workload.bus_messages_per_second = 60;
  workload.chat_updates_per_second = 12;
  return workload;
}

int FrameInterval(int fps, int per_second) {
  return per_second <= 0 ? 0 : std::max(1, fps / per_second);
}

struct Cadence {
  int input_events;
  int bus_interval;
  int chat_interval;
  int telemetry_interval;
};

Cadence CadenceFor(const Workload& workload) {
  return {workload.input_events_per_frame,
          FrameInterval(workload.fps, workload.bus_messages_per_second),
          FrameInterval(workload.fps, workload.chat_updates_per_second),
          FrameInterval(workload.fps, workload.telemetry_batches_per_second)};
}

bool Due(int frame, int interval) {
  return interval > 0 && frame % interval == 0;
}

const char* const kClasses[] = {
    "com/roblox/client/startup/MainGameActivity",
    "com/roblox/client/startup/NativeHelper",
    "com/roblox/client/RobloxActivity",
    "com/roblox/engine/jni/NativeGLJavaInterface",
    "com/roblox/engine/jni/model/NativeTextBoxInfo",
    "com/roblox/engine/jni/model/DeviceStaticParams",
    "com/roblox/engine/jni/OnAppBridgeNotificationListener",
    "com/roblox/engine/jni/memstorage/Connection",
    "com/roblox/engine/jni/memstorage/Callback",
    "com/roblox/universalapp/messagebus/MessageBus",
    "com/roblox/universalapp/messagebus/Connection",
    "com/roblox/universalapp/messagebus/RawCallback",
    "com/roblox/universalapp/messagebus/RequestHandlerRaw",
    "com/roblox/universalapp/messagebus/RequestHandlerAsyncRaw",
    "com/roblox/protocols/systemdialog/PlatformSystemDialogHandler",
    "android/view/MotionEvent",
    "android/view/KeyEvent",
    "android/view/Surface",
    "android/content/Context",
    "android/os/Build",
    "java/lang/String",
    "java/lang/Object",
    "java/util/HashMap",
    "java/util/ArrayList",
    "org/fmod/AudioDevice",
};

const char* const kFieldNames[] = {
    "mNativePointerHandle", "mSurfaceHolderCallback", "mWidth", "mHeight",
    "mDensityDpi",          "mAction",                "mX",     "mY",
    "mEventTime",           "mPointerId",             "mText",  "mSelectionStart",
    "mSelectionEnd",        "mIsMultiline",           "mFocused",
};

std::uint64_t NowNs(clockid_t clock) {
  timespec ts{};
  clock_gettime(clock, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Reads VmRSS or VmHWM (peak) from /proc/self/status, in KiB.
std::uint64_t StatusKiB(const char* key) {
  std::uint64_t value = 0;
  if (FILE* status = std::fopen("/proc/self/status", "r")) {
    char line[128];
    const std::size_t key_length = std::strlen(key);
    while (std::fgets(line, sizeof(line), status) != nullptr) {
      if (std::strncmp(line, key, key_length) == 0 && line[key_length] == ':') {
        value = std::strtoull(line + key_length + 1, nullptr, 10);
        break;
      }
    }
    std::fclose(status);
  }
  return value;
}

struct PhaseSample {
  std::uint64_t wall_ns = 0;
  std::uint64_t cpu_ns = 0;
  std::uint64_t allocations = 0;
  std::uint64_t allocated_bytes = 0;
};

class PhaseTimer {
 public:
  PhaseTimer()
      : wall_(NowNs(CLOCK_MONOTONIC)),
        cpu_(NowNs(CLOCK_PROCESS_CPUTIME_ID)),
        allocations_(g_allocations.load(std::memory_order_relaxed)),
        bytes_(g_allocated_bytes.load(std::memory_order_relaxed)) {}

  PhaseSample Stop() const {
    return {NowNs(CLOCK_MONOTONIC) - wall_,
            NowNs(CLOCK_PROCESS_CPUTIME_ID) - cpu_,
            g_allocations.load(std::memory_order_relaxed) - allocations_,
            g_allocated_bytes.load(std::memory_order_relaxed) - bytes_};
  }

 private:
  std::uint64_t wall_;
  std::uint64_t cpu_;
  std::uint64_t allocations_;
  std::uint64_t bytes_;
};

struct Handles {
  std::vector<jclass> classes;
  std::vector<jfieldID> fields;
  std::vector<jmethodID> methods;
  std::vector<jobject> globals;
  jclass motion_event = nullptr;
  jclass string_class = nullptr;
  jobject activity = nullptr;
  jobject text_box = nullptr;
  jobject audio_device = nullptr;
  jmethodID audio_write = nullptr;
};

jfieldID Field(const Handles& handles, int index) {
  return handles.fields[static_cast<std::size_t>(index)];
}

// Class, method and field lookups, config strings and the global refs that
// native code keeps for the whole session.
void SimulateStartup(JNIEnv* env, Handles& handles) {
  for (const char* name : kClasses) {
    jclass cls = env->FindClass(name);
    handles.classes.push_back(cls);
    handles.globals.push_back(env->NewGlobalRef(cls));
    for (int m = 0; m < 8; ++m) {
      char method[16];
      std::snprintf(method, sizeof(method), "method%d", m);
      handles.methods.push_back(env->GetMethodID(cls, method, "()V"));
    }
  }
  handles.motion_event = handles.classes[15];
  handles.string_class = handles.classes[20];
  for (const char* name : kFieldNames) {
    handles.fields.push_back(env->GetFieldID(handles.classes[0], name, "I"));
  }
  for (int i = 0; i < 300; ++i) {
    char value[48];
    std::snprintf(value, sizeof(value), "FFlagStartupConfigValue%d=true", i);
    jstring str = env->NewStringUTF(value);
    if (i % 10 == 0) handles.globals.push_back(env->NewGlobalRef(str));
    env->DeleteLocalRef(str);
  }

  handles.activity = env->NewGlobalRef(env->AllocObject(handles.classes[0]));
  handles.text_box = env->NewGlobalRef(env->AllocObject(handles.classes[4]));
  handles.globals.push_back(handles.activity);
  handles.globals.push_back(handles.text_box);
  env->SetIntField(handles.activity, Field(handles, 2), 1920);
  env->SetIntField(handles.activity, Field(handles, 3), 1080);
  env->SetIntField(handles.activity, Field(handles, 4), 96);
  jobject surface = env->AllocObject(handles.classes[17]);
  env->SetObjectField(handles.activity, Field(handles, 1), surface);
  env->DeleteLocalRef(surface);

  jclass device_class = handles.classes[24];
  handles.audio_device = env->NewGlobalRef(env->NewObject(
      device_class, env->GetMethodID(device_class, "<init>", "()V")));
  handles.globals.push_back(handles.audio_device);
  handles.audio_write = env->GetMethodID(device_class, "write", "([BI)V");
}

// One frame: input events, surface and size reads, message bus traffic and
// a periodic telemetry batch, all inside a local frame like a JNI callback.
void SimulateFrame(JNIEnv* env, const Handles& handles, int frame,
                   const Cadence& cadence, const std::vector<jbyte>& payload) {
  env->PushLocalFrame(32);
  for (int event = 0; event < cadence.input_events; ++event) {
    jobject motion = env->AllocObject(handles.motion_event);
    env->SetIntField(motion, Field(handles, 5), 2);
    env->SetFloatField(motion, Field(handles, 6), 640.0f + frame);
    env->SetFloatField(motion, Field(handles, 7), 360.0f + event);
    env->SetLongField(motion, Field(handles, 8), frame * 16LL);
    env->SetIntField(motion, Field(handles, 9), event);
    volatile float x = env->GetFloatField(motion, Field(handles, 6));
    volatile float y = env->GetFloatField(motion, Field(handles, 7));
    (void)x;
    (void)y;
    env->DeleteLocalRef(motion);
  }

  jobject surface = env->GetObjectField(handles.activity, Field(handles, 1));
  volatile jint width = env->GetIntField(handles.activity, Field(handles, 2));
  volatile jint height = env->GetIntField(handles.activity, Field(handles, 3));
  (void)width;
  (void)height;
  env->DeleteLocalRef(surface);

  if (Due(frame, cadence.bus_interval)) {
    char json[160];
    std::snprintf(json, sizeof(json),
                  "{\"type\":\"AppBridge.Notification\",\"frame\":%d,"
                  "\"payload\":{\"visible\":true,\"focus\":\"game\"}}",
                  frame);
    jstring message = env->NewStringUTF(json);
    const char* chars = env->GetStringUTFChars(message, nullptr);
    volatile jsize length = env->GetStringUTFLength(message);
    (void)length;
    env->ReleaseStringUTFChars(message, chars);
    jbyteArray blob = env->NewByteArray(256);
    env->SetByteArrayRegion(blob, 0, 256, payload.data());
    volatile jsize blob_length = env->GetArrayLength(blob);
    (void)blob_length;
    env->DeleteLocalRef(blob);
    env->DeleteLocalRef(message);
  }

  if (Due(frame, cadence.chat_interval)) {
    env->SetIntField(handles.text_box, Field(handles, 11), frame % 64);
    env->SetIntField(handles.text_box, Field(handles, 12), frame % 64);
    env->SetBooleanField(handles.text_box, Field(handles, 14), JNI_TRUE);
    jstring text = env->NewStringUTF("hello from the chat box");
    env->SetObjectField(handles.text_box, Field(handles, 10), text);
    env->DeleteLocalRef(text);
  }

  if (Due(frame, cadence.telemetry_interval)) {
    jobjectArray batch = env->NewObjectArray(16, handles.string_class, nullptr);
    for (int i = 0; i < 16; ++i) {
      jstring entry = env->NewStringUTF("telemetry.counter.frame_time_ms");
      env->SetObjectArrayElement(batch, i, entry);
      env->DeleteLocalRef(entry);
    }
    env->DeleteLocalRef(batch);
  }
  env->PopLocalFrame(nullptr);
}

struct AudioThread {
  std::mutex mutex;
  std::condition_variable wake;
  int due = 0;
  bool done = false;
  int written = 0;
};

struct RunResult {
  PhaseSample startup;
  PhaseSample session;
  PhaseSample shutdown;
  std::uint64_t frame_p50_ns = 0;
  std::uint64_t frame_p99_ns = 0;
  std::uint64_t frame_max_ns = 0;
  std::uint64_t session_rss_kib = 0;
  int audio_writes = 0;
};

RunResult RunOnce(int frames, const Workload& workload) {
  const Cadence cadence = CadenceFor(workload);
  RunResult result;
  std::vector<std::uint64_t> frame_ns(static_cast<std::size_t>(frames));
  const std::vector<jbyte> payload(256, 7);

  PhaseTimer startup_timer;
  auto vm = std::make_shared<jnivm::VM>();
  JNIEnv* env = vm->GetJNIEnv();
  auto audio = std::make_shared<AudioThread>();
  jnivm::FmodAudioDeviceCallbacks callbacks;
  callbacks.write = [](void* context, const void*, const std::uint8_t*,
                       std::size_t) {
    ++static_cast<AudioThread*>(context)->written;
    return true;
  };
  vm->SetFmodAudioDeviceCallbacks(audio, callbacks);
  Handles handles;
  handles.classes.reserve(std::size(kClasses));
  handles.methods.reserve(std::size(kClasses) * 8);
  handles.fields.reserve(std::size(kFieldNames));
  handles.globals.reserve(64);
  SimulateStartup(env, handles);
  result.startup = startup_timer.Stop();

  JavaVM* java_vm = vm->GetJavaVM();
  const jobject device = handles.audio_device;
  const jmethodID write = handles.audio_write;
  std::thread audio_thread([java_vm, device, write, audio] {
    JNIEnv* audio_env = nullptr;
    java_vm->AttachCurrentThread(reinterpret_cast<void**>(&audio_env),
                                 nullptr);
    jbyteArray block = audio_env->NewByteArray(kAudioBlockBytes);
    int issued = 0;
    for (;;) {
      std::unique_lock<std::mutex> lock(audio->mutex);
      audio->wake.wait(lock, [&] { return audio->done || audio->due > issued; });
      const int target = audio->due;
      const bool done = audio->done;
      lock.unlock();
      for (; issued < target; ++issued) {
        audio_env->CallVoidMethod(device, write, block,
                                  static_cast<jint>(kAudioBlockBytes));
      }
      if (done) break;
    }
    audio_env->DeleteLocalRef(block);
    java_vm->DetachCurrentThread();
  });

  PhaseTimer session_timer;
  for (int frame = 0; frame < frames; ++frame) {
    const std::uint64_t start = NowNs(CLOCK_MONOTONIC);
    SimulateFrame(env, handles, frame, cadence, payload);
    frame_ns[static_cast<std::size_t>(frame)] = NowNs(CLOCK_MONOTONIC) - start;
    const int due = static_cast<int>(static_cast<long long>(frame + 1) *
                                     kAudioWritesPerSecond / workload.fps);
    std::lock_guard<std::mutex> lock(audio->mutex);
    if (due != audio->due) {
      audio->due = due;
      audio->wake.notify_one();
    }
  }
  {
    std::lock_guard<std::mutex> lock(audio->mutex);
    audio->done = true;
  }
  audio->wake.notify_one();
  audio_thread.join();
  result.session = session_timer.Stop();
  result.session_rss_kib = StatusKiB("VmRSS");
  result.audio_writes = audio->written;

  PhaseTimer shutdown_timer;
  for (jobject global : handles.globals) env->DeleteGlobalRef(global);
  vm->ClearFmodAudioDeviceCallbacks();
  vm.reset();
  result.shutdown = shutdown_timer.Stop();

  std::sort(frame_ns.begin(), frame_ns.end());
  result.frame_p50_ns = frame_ns[frame_ns.size() / 2];
  result.frame_p99_ns = frame_ns[frame_ns.size() * 99 / 100];
  result.frame_max_ns = frame_ns.back();
  return result;
}

std::uint64_t Median(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

int Usage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--profile menu|ingame] [--fps N] [--minutes N] "
               "[--frames N] [--repeat N] [--json PATH]\n",
               program);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  Workload workload = MenuWorkload();
  int frames = 0;
  int minutes = -1;  // -1 picks the profile's default.
  int fps_override = 0;
  int repeat = 5;
  const char* json_path = nullptr;
  for (int i = 1; i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (std::strcmp(argv[i], "--profile") == 0 && has_value) {
      const char* name = argv[++i];
      if (std::strcmp(name, "ingame") == 0) {
        workload = InGameWorkload();
      } else if (std::strcmp(name, "menu") != 0) {
        return Usage(argv[0]);
      }
    } else if (std::strcmp(argv[i], "--fps") == 0 && has_value) {
      fps_override = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--minutes") == 0 && has_value) {
      minutes = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--frames") == 0 && has_value) {
      frames = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--repeat") == 0 && has_value) {
      repeat = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--json") == 0 && has_value) {
      json_path = argv[++i];
    } else {
      return Usage(argv[0]);
    }
  }
  if (fps_override > 0) workload.fps = fps_override;
  if (minutes < 0) minutes = std::strcmp(workload.name, "ingame") == 0 ? 60 : 1;
  if (frames == 0) frames = workload.fps * 60 * minutes;
  if (frames < 1 || repeat < 1 || workload.fps < 1) return Usage(argv[0]);

  // Warm-up run so first-touch page faults and lazy init stay out of the
  // measured runs.
  RunOnce(std::min(frames, 120), workload);

  std::map<std::string, std::vector<std::uint64_t>> samples;
  auto add_phase = [&](const std::string& name, const PhaseSample& phase) {
    samples[name + ".wall_us"].push_back(phase.wall_ns / 1000);
    samples[name + ".cpu_us"].push_back(phase.cpu_ns / 1000);
    samples[name + ".allocations"].push_back(phase.allocations);
    samples[name + ".allocated_kib"].push_back(phase.allocated_bytes / 1024);
  };
  int audio_writes = 0;
  for (int run = 0; run < repeat; ++run) {
    const RunResult result = RunOnce(frames, workload);
    add_phase("startup", result.startup);
    add_phase("session", result.session);
    add_phase("shutdown", result.shutdown);
    samples["frame.p50_ns"].push_back(result.frame_p50_ns);
    samples["frame.p99_ns"].push_back(result.frame_p99_ns);
    samples["frame.max_ns"].push_back(result.frame_max_ns);
    const std::uint64_t cpu_per_frame =
        result.session.cpu_ns / static_cast<std::uint64_t>(frames);
    samples["frame.cpu_ns"].push_back(cpu_per_frame);
    // Parts per million of one frame's time budget at the profile's FPS.
    samples["frame.budget_ppm"].push_back(
        cpu_per_frame * static_cast<std::uint64_t>(workload.fps) / 1000);
    samples["frame.allocations_x100"].push_back(
        result.session.allocations * 100 / static_cast<std::uint64_t>(frames));
    samples["memory.session_rss_kib"].push_back(result.session_rss_kib);
    audio_writes = result.audio_writes;
  }
  samples["memory.peak_rss_kib"].push_back(StatusKiB("VmHWM"));

  const double simulated_minutes =
      static_cast<double>(frames) / workload.fps / 60.0;
  std::printf("jnivm session benchmark: profile %s, %d frames "
              "(%.1f min at %d FPS), %d audio writes, median of %d runs\n",
              workload.name, frames, simulated_minutes, workload.fps,
              audio_writes, repeat);
  for (const auto& [name, values] : samples) {
    std::printf("  %-28s %12llu\n", name.c_str(),
                static_cast<unsigned long long>(Median(values)));
  }

  if (json_path != nullptr) {
    FILE* out = std::fopen(json_path, "w");
    if (out == nullptr) {
      std::perror(json_path);
      return 1;
    }
    std::fprintf(out,
                 "{\"benchmark\":\"jnivm_session\",\"profile\":\"%s\","
                 "\"fps\":%d,\"frames\":%d,\"metrics\":{",
                 workload.name, workload.fps, frames);
    bool first = true;
    for (const auto& [name, values] : samples) {
      std::fprintf(out, "%s\"%s\":%llu", first ? "" : ",", name.c_str(),
                   static_cast<unsigned long long>(Median(values)));
      first = false;
    }
    std::fprintf(out, "}}\n");
    std::fclose(out);
  }
  return 0;
}
