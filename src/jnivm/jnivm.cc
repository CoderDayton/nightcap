// Modified by vii from komaruworld/mocktail. See README "About this fork".
#include "jnivm/jnivm.h"

#include "jni_references.h"
#include "jni_method_names.h"
#include "jni_fields.h"
#include "jni_strings.h"

#include "mocktail/audio/fmod_thread_floating_point.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <list>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "runtime/display_size.h"

namespace jnivm {

using namespace internal;

void Class::RegisterMethod(const std::string& method_name,
                           const std::string& signature,
                           MethodCallback callback) {
  const std::string key = method_name + ":" + signature;
  methods_[key] = std::move(callback);
}

const MethodCallback* Class::FindMethod(const std::string& method_name,
                                        const std::string& signature) const {
  const std::string key = method_name + ":" + signature;
  auto it = methods_.find(key);
  if (it == methods_.end()) {
    return nullptr;
  }
  return &it->second;
}

extern "C" {
void* mocktail_gameactivity_on_start_native = nullptr;
void* mocktail_gameactivity_on_resume_native = nullptr;
void* mocktail_gameactivity_on_surface_created_native = nullptr;
void* mocktail_gameactivity_on_surface_changed_native = nullptr;
void* mocktail_gameactivity_on_surface_redraw_needed_native = nullptr;
void* mocktail_gameactivity_on_trim_memory_native = nullptr;
}

namespace {
thread_local JNIEnv* g_thread_local_env = nullptr;
thread_local JNIEnv g_thread_env_storage = {};
thread_local VM* g_thread_vm_instance = nullptr;
thread_local mocktail::audio::FmodThreadFloatingPointMode g_thread_audio_fp_mode;

// Authentication preflight can briefly own a second VM. Keep every live
// owner registered so discarding that candidate cannot disable the VM that
// the runtime retained. Access is serialized by g_jni_state_mutex.
std::vector<VM*> g_live_vms;

bool IsLiveVmLocked(const VM* vm) {
  return std::find(g_live_vms.begin(), g_live_vms.end(), vm) !=
         g_live_vms.end();
}

bool JniVmTraceEnabled() {
  static const bool enabled = std::getenv("MOCKTAIL_JNI_VM_TRACE") != nullptr;
  return enabled;
}

VM* CurrentVM() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (IsLiveVmLocked(g_thread_vm_instance)) {
    return g_thread_vm_instance;
  }
  return g_live_vms.empty() ? nullptr : g_live_vms.back();
}

PlatformIdentity CurrentPlatformIdentity() {
  VM* vm = CurrentVM();
  return vm != nullptr ? vm->GetPlatformIdentitySnapshot() : PlatformIdentity{};
}

bool IsThreadLocalEnvValid() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  return IsLiveVmLocked(g_thread_vm_instance) &&
         g_thread_local_env == &g_thread_env_storage &&
         g_thread_local_env->functions != nullptr;
}

std::list<std::string> g_method_name_storage;
std::list<std::string> g_method_signature_storage;
std::unordered_map<std::string, jmethodID> g_method_ids;
std::unordered_map<jobject, jlong> g_direct_buffer_capacities;
bool g_android_graph_ready = false;
bool g_fmod_initialized = true;
constexpr jlong kLocalStorageUninitializedUser = -2;
constexpr jlong kLocalStorageNoUser = -1;
std::unordered_map<std::string, std::string> g_local_storage_values;
std::unordered_set<jlong> g_local_storage_users;
jlong g_local_storage_current_user = kLocalStorageUninitializedUser;
bool g_cookie_store_loaded = false;
std::string g_cookie_header;

bool EnvironmentTraceEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool StringTraceEnabled() {
  static const bool enabled =
      EnvironmentTraceEnabled("MOCKTAIL_JNI_STRING_TRACE") ||
      EnvironmentTraceEnabled("MOCKTAIL_TRACE_ALL") ||
      EnvironmentTraceEnabled("MOCKTAIL_FULL_TRACE");
  return enabled;
}

void Trace(const char* name) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] " << name << '\n';
  }
}

struct JniMethodDescriptor {
  const char* name = nullptr;
  const char* signature = nullptr;
  JniMethod method = JniMethod::kUnknown;
};

static std::list<JniMethodDescriptor> g_method_descriptors;

const char* MethodName(jmethodID method_id) {
  if (__builtin_expect(method_id == nullptr, 0)) {
    return "unknown";
  }
  const auto* desc = reinterpret_cast<const JniMethodDescriptor*>(method_id);
  return desc->name != nullptr ? desc->name : "unknown";
}

JniMethod MethodOf(jmethodID method_id) {
  if (__builtin_expect(method_id == nullptr, 0)) {
    return JniMethod::kUnknown;
  }
  return reinterpret_cast<const JniMethodDescriptor*>(method_id)->method;
}

const char* MethodSignature(jmethodID method_id) {
  if (__builtin_expect(method_id == nullptr, 0)) {
    return "";
  }
  const auto* desc = reinterpret_cast<const JniMethodDescriptor*>(method_id);
  return desc->signature != nullptr ? desc->signature : "";
}

thread_local bool t_call_unhandled = false;

void MarkCallUnhandled() { t_call_unhandled = true; }

bool IsGetterName(std::string_view name) {
  return (name.size() > 3 && name.compare(0, 3, "get") == 0) ||
         (name.size() > 2 && name.compare(0, 2, "is") == 0);
}

// Prints each receiver class, method and signature that reached no handler
// once. Getter names are skipped: they read fields, and zero is a valid
// field value.
void ReportUnhandledCall(jobject receiver, jmethodID method_id,
                         bool is_static) {
  const char* name = MethodName(method_id);
  if (method_id == nullptr || IsGetterName(name)) {
    return;
  }
  std::shared_ptr<Class> cls;
  std::string_view class_name;
  if (is_static) {
    cls = ClassFromJClass(static_cast<jclass>(receiver));
    class_name = cls != nullptr ? std::string_view(cls->GetName()) : "";
  } else {
    class_name = ObjectClassName(receiver);
  }
  // Method ids are unique per name and signature, so the pair is the key.
  const std::size_t key = std::hash<std::string_view>{}(class_name) * 31u ^
                          std::hash<const void*>{}(method_id);
  static std::mutex mutex;
  static auto* const reported = new std::unordered_set<std::size_t>();
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!reported->insert(key).second) {
      return;
    }
  }
  std::fprintf(stderr, "  [JNI] no handler for %s%.*s.%s%s\n",
               is_static ? "static " : "",
               class_name.empty() ? 1 : static_cast<int>(class_name.size()),
               class_name.empty() ? "?" : class_name.data(), name,
               MethodSignature(method_id));
}

class UnhandledCallScope {
 public:
  UnhandledCallScope(jobject receiver, jmethodID method_id, bool is_static)
      : receiver_(receiver),
        method_id_(method_id),
        is_static_(is_static),
        outer_(t_call_unhandled) {
    t_call_unhandled = false;
  }
  UnhandledCallScope(const UnhandledCallScope&) = delete;
  UnhandledCallScope& operator=(const UnhandledCallScope&) = delete;
  ~UnhandledCallScope() {
    if (t_call_unhandled) {
      ReportUnhandledCall(receiver_, method_id_, is_static_);
    }
    t_call_unhandled = outer_;
  }

 private:
  jobject receiver_;
  jmethodID method_id_;
  bool is_static_;
  bool outer_;
};

jmethodID StoreMethodId(const char* name, const char* sig) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!name) {
    return nullptr;
  }
  std::string key = name;
  key += ':';
  key += sig ? sig : "";
  auto it = g_method_ids.find(key);
  if (it != g_method_ids.end()) {
    return it->second;
  }
  g_method_name_storage.emplace_back(name);
  g_method_signature_storage.emplace_back(sig ? sig : "");
  const char* stored_name = g_method_name_storage.back().c_str();
  const char* stored_signature = g_method_signature_storage.back().c_str();
  g_method_descriptors.push_back(
      {stored_name, stored_signature, LookupJniMethod(stored_name)});
  jmethodID method_id =
      reinterpret_cast<jmethodID>(&g_method_descriptors.back());
  g_method_ids[key] = method_id;
  return method_id;
}

jobject ObjectResultForMethod(jmethodID method_id);
std::string CookieHeaderForJava();

std::string GetterFieldName(const char* method_name) {
  if (!method_name || method_name[0] == '\0') {
    return "";
  }

  std::string field_name = method_name;
  if (field_name.rfind("get", 0) == 0 && field_name.size() > 3) {
    field_name.erase(0, 3);
  } else if (field_name.rfind("is", 0) == 0 && field_name.size() > 2) {
    field_name.erase(0, 2);
  }

  if (!field_name.empty()) {
    field_name[0] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(field_name[0])));
  }
  return field_name;
}

bool ResolvedDarkTheme() {
  const char* theme = std::getenv("MOCKTAIL_RESOLVED_THEME_INTERNAL");
  return theme == nullptr || std::strcmp(theme, "Light") != 0;
}

jint IntResultForName(jmethodID method_id) {
  const JniMethod method = MethodOf(method_id);
  if (method == JniMethod::kGetScreenWidth) {
    return mocktail::runtime::ParseDisplaySize(
               std::getenv(mocktail::runtime::kDisplaySizeEnvironment))
        .width;
  }
  if (method == JniMethod::kGetScreenHeight) {
    return mocktail::runtime::ParseDisplaySize(
               std::getenv(mocktail::runtime::kDisplaySizeEnvironment))
        .height;
  }
  if (method == JniMethod::kGetWidth) {
    return mocktail::runtime::ParseDisplaySize(
               std::getenv(mocktail::runtime::kWindowSizeEnvironment))
        .width;
  }
  if (method == JniMethod::kGetHeight) {
    return mocktail::runtime::ParseDisplaySize(
               std::getenv(mocktail::runtime::kWindowSizeEnvironment))
        .height;
  }
  if (method == JniMethod::kGetDensityDpi) {
    return 160;
  }
  if (method == JniMethod::kGetApiVersion ||
      method == JniMethod::kGetAndroidApiVersion ||
      method == JniMethod::kGetTargetSdkVersion) {
    return 33;
  }
  if (method == JniMethod::kGetRefreshRate) {
    return 60;
  }
  if (method == JniMethod::kGetSdkVersion) {
    return 33;
  }
  if (method == JniMethod::kGetOutputSampleRate) {
    return 48000;
  }
  if (method == JniMethod::kGetOutputBlockSize) {
    return 512;
  }
  if (method == JniMethod::kGetSystemTheme) {
    return ResolvedDarkTheme() ? 4 : 3;
  }
  if (method == JniMethod::kGetRotation ||
      method == JniMethod::kGetInt ||
      method == JniMethod::kGetMode) {
    return 0;
  }
  MarkCallUnhandled();
  return 0;
}

jint IntResultForMethod(jmethodID method_id) {
  return IntResultForName(method_id);
}

jint IdentityHashCode(jobject obj) {
  uintptr_t value = reinterpret_cast<uintptr_t>(obj);
  value ^= value >> 32;
  value &= 0x7fffffff;
  return value == 0 ? 1 : static_cast<jint>(value);
}

jint StaticIntResultForMethodV(jmethodID method_id, va_list args) {
  const JniMethod method = MethodOf(method_id);
  if (method == JniMethod::kIdentityHashCode) {
    jobject obj = va_arg(args, jobject);
    return IdentityHashCode(obj);
  }
  return IntResultForMethod(method_id);
}

jint StaticIntResultForMethodA(jmethodID method_id, const jvalue* args) {
  const JniMethod method = MethodOf(method_id);
  if (method == JniMethod::kIdentityHashCode) {
    return IdentityHashCode(args ? args[0].l : nullptr);
  }
  return IntResultForMethod(method_id);
}

jlong CurrentTimeMillis() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<jlong>(ts.tv_sec) * 1000 +
         static_cast<jlong>(ts.tv_nsec / 1000000);
}

jlong NanoTime() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<jlong>(ts.tv_sec) * 1000000000LL +
         static_cast<jlong>(ts.tv_nsec);
}

RobloxAuthIdentity ResearchRobloxIdentityFromEnvironment() {
  RobloxAuthIdentity identity;
  const char* value = std::getenv("MOCKTAIL_ROBLOX_USER_ID");
  if (!value || value[0] == '\0') {
    return identity;
  }

  errno = 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0' || parsed <= 0 ||
      static_cast<unsigned long long>(parsed) >
          static_cast<unsigned long long>(
              std::numeric_limits<int64_t>::max())) {
    return identity;
  }

  // Normal startup injects identity through VM::SetRobloxAuthIdentity.
  identity.user_id = static_cast<int64_t>(parsed);
  const char* username = std::getenv("MOCKTAIL_ROBLOX_USERNAME");
  if (username && username[0] != '\0') {
    identity.username = username;
    identity.display_name = username;
  }
  return identity;
}

RobloxAuthIdentity RobloxIdentityForJava() {
  VM* vm = CurrentVM();
  if (vm) {
    RobloxAuthIdentity identity = vm->GetRobloxAuthIdentitySnapshot();
    if (identity.user_id > 0) {
      return identity;
    }
  }
  return ResearchRobloxIdentityFromEnvironment();
}

jlong RobloxUserIdForJava() {
  return static_cast<jlong>(RobloxIdentityForJava().user_id);
}

jlong StaticLongResultForMethod(jmethodID method_id) {
  const JniMethod method = MethodOf(method_id);
  if (method == JniMethod::kCurrentTimeMillis) {
    return CurrentTimeMillis();
  }
  if (method == JniMethod::kNanoTime) {
    return NanoTime();
  }
  if (method == JniMethod::kGetUserId) {
    return RobloxUserIdForJava();
  }
  MarkCallUnhandled();
  return 0;
}

jobject ExactMessageBusStaticObject(jclass clazz, jmethodID method_id) {
  const std::shared_ptr<Class> object_class = ClassFromJClass(clazz);
  if (object_class == nullptr ||
      object_class->GetName() !=
          "com/roblox/universalapp/messagebus/MessageBus" ||
      MethodOf(method_id) != JniMethod::kF ||
      std::strcmp(MethodSignature(method_id),
                  "()Lcom/roblox/universalapp/messagebus/MessageBus;") != 0) {
    return nullptr;
  }
  return SingletonObject("com/roblox/universalapp/messagebus/MessageBus");
}

jobject EngineJavaCallbackObject() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  jobject callback = StaticObjectFieldValue("sImplementation");
  if (callback == nullptr) {
    callback = MakeObjectForClass("com/roblox/engine/jni/EngineJavaCallback2");
    // VM-owned like a singleton, so detaching the creating thread keeps it.
    MarkSingletonHandle(callback);
    SetStaticObjectFieldRaw("sImplementation", callback);
  }
  return callback;
}

void SetStringFieldRaw(jobject obj, const char* field_name, const char* value);

void SetStringFieldRaw(jobject obj, const char* field_name, const char* value) {
  const jstring string = MakeString(value);
  SetObjectFieldRaw(obj, field_name, string);
  DeleteLocalJniReference(string);
}

void RecordAppBridgeNotification(jobject obj, jstring type, jstring data) {
  const std::string type_value = StringFromJString(type);
  const std::string data_value = StringFromJString(data);
  SetStringFieldRaw(obj, "lastAppBridgeNotificationType", type_value.c_str());
  SetStringFieldRaw(obj, "lastAppBridgeNotificationData", data_value.c_str());
  if (type_value == "APP_READY") {
    SetBooleanFieldRaw(obj, "appReady", JNI_TRUE);
  }
  if (TraceEnabled()) {
    std::cout << "  [JNI callback] OnAppBridgeNotificationListener.a type="
              << type_value << " data_bytes=" << data_value.size() << '\n';
  }
}

void ForwardDataModelNotificationToAppBridgeListener(jstring type,
                                                     jstring data) {
  jobject listener = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    listener = StaticObjectFieldValue("sAppBridgeNotificationListener");
  }
  if (listener == nullptr) {
    return;
  }
  RecordAppBridgeNotification(listener, type, data);
  DeleteLocalJniReference(listener);
}

void HandleRobloxDidSwitchAccount(jobject obj, const char* name);
void HandleRobloxDidLogOut(jobject obj, const char* name);

void RecordDataModelNotification(jobject obj, jstring type, jstring data) {
  const std::string type_value = StringFromJString(type);
  const std::string data_value = StringFromJString(data);
  SetStringFieldRaw(obj, "lastDataModelNotificationType", type_value.c_str());
  SetStringFieldRaw(obj, "lastDataModelNotificationData", data_value.c_str());
  if (type_value == "APP_READY") {
    SetBooleanFieldRaw(obj, "appReady", JNI_TRUE);
  }
  if (TraceEnabled()) {
    std::cout << "  [JNI callback] EngineJavaCallback2.f type=" << type_value
              << " data=" << data_value << '\n';
  }
  VM *vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxDataModelNotification(vm->GetJNIEnv(), type, data);
  }
  ForwardDataModelNotificationToAppBridgeListener(type, data);
  if (type_value == "DID_SWITCH_ACCOUNT" || type_value == "DID_LOG_OUT") {
    jobject helper = SingletonObject("com/roblox/client/startup/NativeHelper");
    if (type_value == "DID_SWITCH_ACCOUNT") {
      HandleRobloxDidSwitchAccount(helper,
                                   "gameActivity_onDidSwitchAccountReceived");
    } else {
      HandleRobloxDidLogOut(helper, "gameActivity_onDidLogOutReceived");
    }
  }
}

jobject MakeNativeHelperObject(jobject activity) {
  jobject helper = SingletonObject("com/roblox/client/startup/NativeHelper");
  jobject helper_activity =
      activity ? activity
               : SingletonObject("com/roblox/client/startup/MainGameActivity");
  SetObjectFieldRaw(helper, "activity", helper_activity);
  SetObjectFieldRaw(helper, "a", helper_activity);
  SetBooleanFieldRaw(helper, "isVisible", JNI_TRUE);
  SetBooleanFieldRaw(helper, "g", JNI_TRUE);
  SetBooleanFieldRaw(helper, "isForeground", JNI_TRUE);
  SetBooleanFieldRaw(helper, "h", JNI_TRUE);
  SetBooleanFieldRaw(helper, "isEngineInitialized", JNI_FALSE);
  SetBooleanFieldRaw(helper, "i", JNI_FALSE);
  SetBooleanFieldRaw(helper, "isInExperience", JNI_FALSE);
  SetBooleanFieldRaw(helper, "j", JNI_FALSE);
  SetIntFieldRaw(helper, "contentViewId", 0);
  SetIntFieldRaw(helper, "m", 0);
  return helper;
}

void SetNativeHelperBoolean(jobject helper,
                            const char* field_name,
                            const char* obfuscated_field_name,
                            jboolean value) {
  SetBooleanFieldRaw(helper, field_name, value);
  SetBooleanFieldRaw(helper, obfuscated_field_name, value);
}

void RecordNativeHelperCallback(jobject obj, const char* name) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] NativeHelper callback: " << name << '\n';
  }
  SetBooleanFieldRaw(obj, name, JNI_TRUE);
}

bool IsJavaStringGetBytesMethod(jobject obj, jmethodID method_id) {
  if (obj == nullptr || method_id == nullptr ||
      ObjectClassName(obj) != "java/lang/String" ||
      MethodOf(method_id) != JniMethod::kGetBytes ||
      std::strcmp(MethodSignature(method_id),
                  "(Ljava/lang/String;)[B") != 0) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  return g_known_strings.find(reinterpret_cast<jstring>(obj)) !=
         g_known_strings.end();
}

std::string TrimString(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::string ReadTextFile(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  std::ifstream file(path);
  if (!file) {
    return {};
  }
  std::string content;
  std::string line;
  while (std::getline(file, line)) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += line;
  }
  return content;
}

std::string HomePath(const char* suffix) {
  const char* home = std::getenv("HOME");
  if (!home || home[0] == '\0') {
    return {};
  }
  std::string path = home;
  path += suffix ? suffix : "";
  return path;
}

std::string CookieValueAfterEquals(const std::string& text,
                                   std::size_t name_pos) {
  std::size_t equals = text.find('=', name_pos);
  if (equals == std::string::npos) {
    return {};
  }
  std::size_t start = equals + 1;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  std::size_t end = text.find_first_of(";\r\n\t", start);
  return TrimString(text.substr(start, end == std::string::npos
                                           ? std::string::npos
                                           : end - start));
}

std::string NormalizeCookieHeader(const std::string& content) {
  constexpr const char* kCookieName = ".ROBLOSECURITY";
  constexpr const char* kCookiePrefix = ".ROBLOSECURITY=";
  std::string text = TrimString(content);
  if (text.empty()) {
    return {};
  }
  std::size_t prefixed_pos = text.find(kCookiePrefix);
  if (prefixed_pos != std::string::npos) {
    std::string value = CookieValueAfterEquals(text, prefixed_pos);
    return value.empty() ? std::string() : std::string(kCookiePrefix) + value;
  }
  std::size_t name_pos = text.find(kCookieName);
  if (name_pos != std::string::npos) {
    const std::size_t after_name = name_pos + std::strlen(kCookieName);
    if (after_name < text.size() && text[after_name] == '\t') {
      std::size_t end = text.find_first_of(";\r\n\t", after_name + 1);
      std::string value = TrimString(text.substr(
          after_name + 1,
          end == std::string::npos ? std::string::npos : end - after_name - 1));
      return value.empty() ? std::string() : std::string(kCookiePrefix) + value;
    }
    std::string value = CookieValueAfterEquals(text, name_pos);
    return value.empty() ? std::string() : std::string(kCookiePrefix) + value;
  }
  if (text.find('=') == std::string::npos &&
      text.find('\t') == std::string::npos &&
      text.find(';') == std::string::npos) {
    return std::string(kCookiePrefix) + text;
  }
  return {};
}

constexpr jsize kMaximumCookieSetCount = 128;
constexpr jsize kMaximumCookieSetBytes = 64 * 1024;

std::string CookieValueFromHeader(const std::string& header) {
  constexpr const char* kCookiePrefix = ".ROBLOSECURITY=";
  std::size_t pos = header.find(kCookiePrefix);
  if (pos == std::string::npos) {
    return {};
  }
  return CookieValueAfterEquals(header, pos);
}

void ClearCookieString(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* byte = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    byte[index] = '\0';
  }
  value->clear();
}

void ClearLegacyCookieStore() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  ClearCookieString(&g_cookie_header);
  g_cookie_store_loaded = false;
}

std::string MocktailConfigRoot() {
  const char* root = std::getenv("MOCKTAIL_CONFIG_ROOT");
  if (root && root[0] != '\0') {
    return root;
  }
  return HomePath("/.config/mocktail");
}

void EnsureCookieStoreLoadedLocked() {
  if (g_cookie_store_loaded) {
    return;
  }
  g_cookie_store_loaded = true;

  const char* env_cookie = std::getenv("MOCKTAIL_ROBLOX_COOKIES");
  g_cookie_header = NormalizeCookieHeader(env_cookie ? env_cookie : "");
  if (g_cookie_header.empty()) {
    const char* cookie_file = std::getenv("MOCKTAIL_COOKIE_FILE");
    if (cookie_file && cookie_file[0] != '\0') {
      g_cookie_header = NormalizeCookieHeader(ReadTextFile(cookie_file));
    }
  }
  if (g_cookie_header.empty()) {
    g_cookie_header =
        NormalizeCookieHeader(ReadTextFile(MocktailConfigRoot() + "/cookie"));
  }
  if (TraceEnabled()) {
    std::cout << "  [JNI] cookie store loaded bytes="
              << g_cookie_header.size() << '\n';
  }
}

void StoreCookieHeader(const std::string& cookie) {
  VM* vm = CurrentVM();
  std::string normalized = NormalizeCookieHeader(cookie);
  if (normalized.empty()) {
    return;
  }
  if (vm != nullptr) {
    (void)vm->DispatchRobloxCredential(normalized.data(), normalized.size());
    if (vm->CopyRobloxCredentialFromProvider(nullptr)) {
      ClearCookieString(&normalized);
      return;
    }
  }
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_cookie_header = normalized;
    g_cookie_store_loaded = true;
  }
  ClearCookieString(&normalized);
}

std::string CookieHeaderForJava() {
  std::string credential;
  VM* vm = CurrentVM();
  if (vm != nullptr && vm->CopyRobloxCredentialFromProvider(&credential)) {
    return credential;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  EnsureCookieStoreLoadedLocked();
  return g_cookie_header;
}

std::string CookieNetscapeForJava(const std::string& domain_or_url) {
  std::string header = CookieHeaderForJava();
  std::string value = CookieValueFromHeader(header);
  ClearCookieString(&header);
  if (value.empty()) {
    return {};
  }
  std::string domain = domain_or_url;
  std::size_t scheme = domain.find("://");
  if (scheme != std::string::npos) {
    domain = domain.substr(scheme + 3);
  }
  std::size_t slash = domain.find('/');
  if (slash != std::string::npos) {
    domain = domain.substr(0, slash);
  }
  if (domain.empty()) {
    domain = "roblox.com";
  }
  if (domain.front() != '.') {
    domain = "." + domain;
  }
  std::string result =
      domain + "\tTRUE\t/\tTRUE\t2147483647\t.ROBLOSECURITY\t" + value;
  ClearCookieString(&value);
  return result;
}

jobject MakeCookieStringForJava(std::string cookie) {
  jobject result = MakeString(cookie.c_str());
  ClearCookieString(&cookie);
  return result;
}

bool CookieAvailableForJava() {
  std::string cookie = CookieHeaderForJava();
  const bool available = !cookie.empty();
  ClearCookieString(&cookie);
  return available;
}

bool SyncRobloxCookiesFromEngine(VM* vm) {
  if (vm == nullptr) {
    return false;
  }
  JNIEnv* env = vm->GetJNIEnv();
  const bool refreshed = vm->RefreshRobloxCredentialFromEngine(env);
  std::string header = CookieHeaderForJava();
  jstring cookie = static_cast<jstring>(MakeString(header.c_str()));
  (void)vm->DispatchRobloxCookieSync(env, cookie);
  ClearCookieString(&header);
  return refreshed;
}

bool IsRobloxCookieSyncMethod(jobject obj, jmethodID method_id) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == JniMethod::kSyncCookiesFromEngine &&
         std::strcmp(MethodSignature(method_id), "()V") == 0 &&
         ObjectClassName(obj) == "com/roblox/client/startup/MainGameActivity";
}

bool IsRobloxDidLogInMethod(jobject obj, jmethodID method_id) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == JniMethod::kGameActivityOnDidLogInReceived &&
         std::strcmp(MethodSignature(method_id), "(Ljava/lang/String;)V") == 0 &&
         ObjectClassName(obj) == "com/roblox/client/startup/NativeHelper";
}

bool IsNativeHelperNotification(jobject obj, jmethodID method_id,
                                JniMethod method) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), "()V") == 0 &&
         ObjectClassName(obj) == "com/roblox/client/startup/NativeHelper";
}

bool IsRobloxDidSwitchAccountMethod(jobject obj, jmethodID method_id) {
  return IsNativeHelperNotification(
      obj, method_id, JniMethod::kGameActivityOnDidSwitchAccountReceived);
}

bool IsRobloxDidLogOutMethod(jobject obj, jmethodID method_id) {
  return IsNativeHelperNotification(
      obj, method_id, JniMethod::kGameActivityOnDidLogOutReceived);
}

void HandleRobloxDidSwitchAccount(jobject obj, const char* name) {
  VM* vm = CurrentVM();
  const bool refreshed = SyncRobloxCookiesFromEngine(vm);
  RecordNativeHelperCallback(obj, name);
  if (vm != nullptr) {
    vm->DispatchRobloxAccountSwitch(refreshed);
  }
}

void HandleRobloxDidLogOut(jobject obj, const char* name) {
  VM* vm = CurrentVM();
  const bool was_signed_in =
      vm != nullptr && vm->GetRobloxAuthIdentitySnapshot().user_id > 0;
  std::string before = CookieHeaderForJava();
  std::string signed_out_value = CookieValueFromHeader(before);
  ClearCookieString(&before);
  bool engine_signed_in = SyncRobloxCookiesFromEngine(vm);
  if (engine_signed_in && !signed_out_value.empty()) {
    std::string after = CookieHeaderForJava();
    std::string after_value = CookieValueFromHeader(after);
    engine_signed_in = after_value != signed_out_value;
    ClearCookieString(&after);
    ClearCookieString(&after_value);
  }
  ClearCookieString(&signed_out_value);
  RecordNativeHelperCallback(obj, name);
  if (engine_signed_in || was_signed_in) {
    vm->DispatchRobloxAccountSwitch(engine_signed_in);
  }
}

bool CookieObjectResultForMethodV(jmethodID method_id, va_list args,
                                  jobject* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kGetCookie ||
      method == JniMethod::kNativeGetCookiesForDomain) {
    (void)va_arg(args, jstring);
    *result = MakeCookieStringForJava(CookieHeaderForJava());
    return true;
  }
  if (method == JniMethod::kNativeGetCookiesInNetscapeFormat) {
    jstring domain = va_arg(args, jstring);
    *result = MakeCookieStringForJava(
        CookieNetscapeForJava(StringFromJString(domain)));
    return true;
  }
  return false;
}

bool CookieObjectResultForMethodA(jmethodID method_id, const jvalue* args,
                                  jobject* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kGetCookie ||
      method == JniMethod::kNativeGetCookiesForDomain) {
    *result = MakeCookieStringForJava(CookieHeaderForJava());
    return true;
  }
  if (method == JniMethod::kNativeGetCookiesInNetscapeFormat) {
    std::string domain =
        args ? StringFromJString(reinterpret_cast<jstring>(args[0].l)) : "";
    *result = MakeCookieStringForJava(CookieNetscapeForJava(domain));
    return true;
  }
  return false;
}

bool CookieBooleanResultForMethod(jmethodID method_id, jboolean* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kSetCookiesFromDisk) {
    *result = CookieAvailableForJava() ? JNI_TRUE : JNI_FALSE;
    return true;
  }
  return false;
}

bool FmodBooleanResultForMethod(jmethodID method_id, jboolean* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kCheckInit) {
    *result = g_fmod_initialized ? JNI_TRUE : JNI_FALSE;
    return true;
  }
  if (method == JniMethod::kSupportsAAudio ||
      method == JniMethod::kSupportsLowLatency ||
      method == JniMethod::kLowLatencyFlag ||
      method == JniMethod::kProAudioFlag ||
      method == JniMethod::kIsBluetoothOn) {
    *result = JNI_FALSE;
    return true;
  }
  return false;
}

jlong ParseLocalStorageUserIdFromEnv() {
  const char* value = std::getenv("MOCKTAIL_LOCAL_STORAGE_CURRENT_USER_ID");
  if (!value || value[0] == '\0') {
    value = std::getenv("MOCKTAIL_ROBLOX_USER_ID");
  }
  if (!value || value[0] == '\0') {
    return 0;
  }
  errno = 0;
  char* end = nullptr;
  long long parsed = std::strtoll(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0') {
    return 0;
  }
  return static_cast<jlong>(parsed);
}

jlong CurrentLocalStorageUserLocked() {
  if (g_local_storage_current_user == kLocalStorageUninitializedUser) {
    g_local_storage_current_user = ParseLocalStorageUserIdFromEnv();
    if (g_local_storage_current_user != kLocalStorageNoUser) {
      g_local_storage_users.insert(g_local_storage_current_user);
    }
  }
  return g_local_storage_current_user;
}

std::string LocalStorageUserKey(const std::string& key, jlong user_id) {
  return std::to_string(user_id) + "_" + key;
}

bool CommaListContains(const std::string& list, const std::string& value) {
  std::size_t start = 0;
  while (start <= list.size()) {
    std::size_t end = list.find(',', start);
    std::string item =
        list.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    if (item == value) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

void AppendCommaListValue(std::string* list, const std::string& value) {
  if (!list || value.empty() || CommaListContains(*list, value)) {
    return;
  }
  if (!list->empty()) {
    list->push_back(',');
  }
  *list += value;
}

void RefreshLocalStorageUsersValueLocked() {
  std::string users;
  for (jlong user_id : g_local_storage_users) {
    AppendCommaListValue(&users, std::to_string(user_id));
  }
  g_local_storage_values["Users"] = users;
}

std::string LocalStorageGetValue(const std::string& key) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  CurrentLocalStorageUserLocked();
  auto it = g_local_storage_values.find(key);
  return it == g_local_storage_values.end() ? std::string() : it->second;
}

void LocalStorageSetUserValueLocked(jlong user_id, const std::string& key,
                                    const std::string& value) {
  g_local_storage_users.insert(user_id);
  g_local_storage_values[LocalStorageUserKey(key, user_id)] = value;
  std::string& user_keys =
      g_local_storage_values[LocalStorageUserKey("UserKeys", user_id)];
  AppendCommaListValue(&user_keys, key);
  RefreshLocalStorageUsersValueLocked();
}

bool LocalStorageDeleteUserValues(jlong user_id) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  CurrentLocalStorageUserLocked();
  std::string prefix = std::to_string(user_id) + "_";
  for (auto it = g_local_storage_values.begin();
       it != g_local_storage_values.end();) {
    if (it->first.rfind(prefix, 0) == 0) {
      it = g_local_storage_values.erase(it);
    } else {
      ++it;
    }
  }
  g_local_storage_users.erase(user_id);
  if (g_local_storage_current_user == user_id) {
    g_local_storage_current_user = kLocalStorageNoUser;
  }
  RefreshLocalStorageUsersValueLocked();
  return JNI_TRUE;
}

bool LocalStorageObjectResultForMethodV(jmethodID method_id, va_list args,
                                        jobject* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kGetSecureValue) {
    jstring key = va_arg(args, jstring);
    *result = MakeString(LocalStorageGetValue(StringFromJString(key)).c_str());
    return true;
  }
  if (method == JniMethod::kGetSecureValueForCurrentUser) {
    jstring key = va_arg(args, jstring);
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    std::string value;
    if (user_id != kLocalStorageNoUser) {
      value = LocalStorageGetValue(LocalStorageUserKey(StringFromJString(key),
                                                       user_id));
    }
    *result = MakeString(value.c_str());
    return true;
  }
  if (method == JniMethod::kGetSecureValueForUser) {
    jstring key = va_arg(args, jstring);
    jlong user_id = va_arg(args, jlong);
    *result = MakeString(LocalStorageGetValue(
        LocalStorageUserKey(StringFromJString(key), user_id)).c_str());
    return true;
  }
  if (method == JniMethod::kGetUsers) {
    jobject users = MakeObjectForClass("java/util/HashSet");
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      CurrentLocalStorageUserLocked();
      SetIntFieldRaw(users, "size",
                     static_cast<jint>(g_local_storage_users.size()));
    }
    *result = users;
    return true;
  }
  if (method == JniMethod::kIterator) {
    *result = SingletonObject("java/util/Iterator");
    return true;
  }
  return false;
}

bool LocalStorageObjectResultForMethodA(jmethodID method_id, const jvalue* args,
                                        jobject* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kGetSecureValue) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    *result = MakeString(LocalStorageGetValue(StringFromJString(key)).c_str());
    return true;
  }
  if (method == JniMethod::kGetSecureValueForCurrentUser) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    std::string value;
    if (user_id != kLocalStorageNoUser) {
      value = LocalStorageGetValue(LocalStorageUserKey(StringFromJString(key),
                                                       user_id));
    }
    *result = MakeString(value.c_str());
    return true;
  }
  if (method == JniMethod::kGetSecureValueForUser) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jlong user_id = args ? args[1].j : 0;
    *result = MakeString(LocalStorageGetValue(
        LocalStorageUserKey(StringFromJString(key), user_id)).c_str());
    return true;
  }
  if (method == JniMethod::kGetUsers) {
    jobject users = MakeObjectForClass("java/util/HashSet");
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      CurrentLocalStorageUserLocked();
      SetIntFieldRaw(users, "size",
                     static_cast<jint>(g_local_storage_users.size()));
    }
    *result = users;
    return true;
  }
  if (method == JniMethod::kIterator) {
    *result = SingletonObject("java/util/Iterator");
    return true;
  }
  return false;
}

bool LocalStorageBooleanResultForMethodV(jmethodID method_id, va_list args,
                                         jboolean* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kSetCurrentUser) {
    jlong user_id = va_arg(args, jlong);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_local_storage_current_user = user_id;
    if (user_id != kLocalStorageNoUser) {
      g_local_storage_users.insert(user_id);
    }
    RefreshLocalStorageUsersValueLocked();
    g_local_storage_values["CurrentUser"] = std::to_string(user_id);
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kSetSecureValue) {
    jstring key = va_arg(args, jstring);
    jstring value = va_arg(args, jstring);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values[StringFromJString(key)] = StringFromJString(value);
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kSetSecureValueForCurrentUser) {
    jstring key = va_arg(args, jstring);
    jstring value = va_arg(args, jstring);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    jlong user_id = CurrentLocalStorageUserLocked();
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
      return true;
    }
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kSetSecureValueForUser) {
    jstring key = va_arg(args, jstring);
    jstring value = va_arg(args, jstring);
    jlong user_id = va_arg(args, jlong);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kDeleteSecureValue) {
    jstring key = va_arg(args, jstring);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values.erase(StringFromJString(key));
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kDeleteUserValues) {
    jlong user_id = va_arg(args, jlong);
    *result = LocalStorageDeleteUserValues(user_id);
    return true;
  }
  if (method == JniMethod::kDeleteCurrentUserValues) {
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
    } else {
      *result = LocalStorageDeleteUserValues(user_id);
    }
    return true;
  }
  if (method == JniMethod::kHasNext) {
    *result = JNI_FALSE;
    return true;
  }
  return false;
}

bool LocalStorageBooleanResultForMethodA(jmethodID method_id, const jvalue* args,
                                         jboolean* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kSetCurrentUser) {
    jlong user_id = args ? args[0].j : 0;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_local_storage_current_user = user_id;
    if (user_id != kLocalStorageNoUser) {
      g_local_storage_users.insert(user_id);
    }
    RefreshLocalStorageUsersValueLocked();
    g_local_storage_values["CurrentUser"] = std::to_string(user_id);
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kSetSecureValue) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jstring value = args ? static_cast<jstring>(args[1].l) : nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values[StringFromJString(key)] = StringFromJString(value);
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kSetSecureValueForCurrentUser) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jstring value = args ? static_cast<jstring>(args[1].l) : nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    jlong user_id = CurrentLocalStorageUserLocked();
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
      return true;
    }
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kSetSecureValueForUser) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jstring value = args ? static_cast<jstring>(args[1].l) : nullptr;
    jlong user_id = args ? args[2].j : 0;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kDeleteSecureValue) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values.erase(StringFromJString(key));
    *result = JNI_TRUE;
    return true;
  }
  if (method == JniMethod::kDeleteUserValues) {
    jlong user_id = args ? args[0].j : 0;
    *result = LocalStorageDeleteUserValues(user_id);
    return true;
  }
  if (method == JniMethod::kDeleteCurrentUserValues) {
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
    } else {
      *result = LocalStorageDeleteUserValues(user_id);
    }
    return true;
  }
  if (method == JniMethod::kHasNext) {
    *result = JNI_FALSE;
    return true;
  }
  return false;
}

bool LocalStorageLongResultForMethod(jmethodID method_id, jlong* result) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name || !result) {
    return false;
  }
  if (method == JniMethod::kGetCurrentUser) {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    *result = CurrentLocalStorageUserLocked();
    return true;
  }
  return false;
}

bool HandleAndroidSetWindowFlagsMethodV(jobject obj, jmethodID method_id,
                                        va_list args);

void HandleVoidMethod(jobject obj, jmethodID method_id, va_list args) {
  if (__builtin_expect(method_id == nullptr, 0)) {
    return;
  }
  const auto *desc = reinterpret_cast<const JniMethodDescriptor *>(method_id);
  const JniMethod method = MethodOf(method_id);

  if (HandleAndroidSetWindowFlagsMethodV(obj, method_id, args)) {
    return;
  }

  switch (method) {
    case JniMethod::kRun: {
      if (ObjectClassName(obj) ==
              "com/roblox/universalapp/messagebus/RequestHandlerAsyncRaw" &&
          std::strcmp(MethodSignature(method_id),
                      "(Ljava/lang/String;Ljava/lang/String;)V") == 0) {
        jstring message = va_arg(args, jstring);
        jstring response_id = va_arg(args, jstring);
        VM* vm = CurrentVM();
        if (vm != nullptr) {
          vm->DispatchMessageBusAsyncRequestHandler(obj, vm->GetJNIEnv(),
                                                    message, response_id);
        }
        return;
      }
      if (ObjectClassName(obj) ==
          "com/roblox/universalapp/messagebus/RawCallback") {
        jstring message = va_arg(args, jstring);
        VM *vm = CurrentVM();
        if (vm != nullptr) {
          vm->DispatchMessageBusRawCallback(obj, vm->GetJNIEnv(), message);
        }
        return;
      }
      break;
    }
    case JniMethod::kA: {
      if (ObjectClassName(obj) ==
          "com/roblox/engine/jni/OnAppBridgeNotificationListener") {
        auto type = va_arg(args, jstring);
        auto data = va_arg(args, jstring);
        RecordAppBridgeNotification(obj, type, data);
        return;
      }
      break;
    }
    case JniMethod::kF: {
      if (std::strcmp(MethodSignature(method_id),
                      "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
          ObjectClassName(obj) == "com/roblox/engine/jni/EngineJavaCallback2") {
        auto type = va_arg(args, jstring);
        auto data = va_arg(args, jstring);
        RecordDataModelNotification(obj, type, data);
        return;
      }
      break;
    }
    case JniMethod::kSetBaseUrl: {
      auto value = va_arg(args, jstring);
      std::string base_url = StringFromJString(value);
      if (base_url.empty()) {
        base_url = "https://www.roblox.com";
      }
      SetStringFieldRaw(obj, "baseUrl", base_url.c_str());
      if (TraceEnabled()) {
        std::cout << "  [JNI] setBaseUrl value=" << base_url << '\n';
      }
      return;
    }
    case JniMethod::kSetCookie: {
      auto first = va_arg(args, jstring);
      auto second = va_arg(args, jstring);
      SetStringFieldRaw(obj, "cookieName", StringFromJString(first).c_str());
      SetStringFieldRaw(obj, "cookieValue", StringFromJString(second).c_str());
      StoreCookieHeader(StringFromJString(second));
      return;
    }
    case JniMethod::kBootstrapTheApp: {
      SetBooleanFieldRaw(obj, "bootstrapStarted", JNI_TRUE);
      if (TraceEnabled()) {
        std::cout << "  [JNI] MainGameActivity.bootstrapTheApp no-op; "
                  << "native bootstrap is driven by Mocktail\n";
      }
      return;
    }
    default:
      break;
  }

  const char *name = desc ? desc->name : MethodName(method_id);
  const std::string_view class_name = ObjectClassName(obj);
  if (class_name == "com/roblox/engine/jni/EngineJavaCallback2" &&
      std::strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'o') {
    return;
  }
  if (method == JniMethod::kSetImeEditorInfoFields ||
      method == JniMethod::kSetWindowFlags ||
      method == JniMethod::kRunOnUiThread ||
      method == JniMethod::kRequestOrientationAsDefault ||
      method == JniMethod::kShowLeaveAppPrompt ||
      method == JniMethod::kHideKeyboard ||
      method == JniMethod::kRestartInput ||
      method == JniMethod::kSetSoftKeyboardActive ||
      method == JniMethod::kSetState) {
    return;
  }
  if (IsRobloxCookieSyncMethod(obj, method_id)) {
    SyncRobloxCookiesFromEngine(CurrentVM());
    return;
  }
  if (method == JniMethod::kL0) {
    jint content_view_id = va_arg(args, jint);
    SetIntFieldRaw(obj, "contentViewId", content_view_id);
    SetIntFieldRaw(obj, "m", content_view_id);
    return;
  }
  if (method == JniMethod::kK1) {
    jboolean visible = static_cast<jboolean>(va_arg(args, jint));
    SetNativeHelperBoolean(MakeNativeHelperObject(nullptr), "isVisible", "g",
                           visible);
    return;
  }
  if (method == JniMethod::kGameActivityOnEngineInitialized) {
    SetNativeHelperBoolean(obj, "isEngineInitialized", "i", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnFlagsLoaded) {
    (void)va_arg(args, jobject);
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_TRUE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnFlagsFailed) {
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_FALSE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnGameLoaded) {
    jlong place_id = va_arg(args, jlong);
    SetLongFieldRaw(obj, "placeId", place_id);
    SetNativeHelperBoolean(obj, "isInExperience", "j",
                           place_id > 0 ? JNI_TRUE : JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnExperienceStart) {
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnExperienceStop) {
    (void)va_arg(args, double);
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnScreenOrientationChanged) {
    jint orientation = va_arg(args, jint);
    jboolean in_experience = static_cast<jboolean>(va_arg(args, jint));
    SetIntFieldRaw(obj, "orientation", orientation);
    SetNativeHelperBoolean(obj, "isInExperience", "j", in_experience);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (IsRobloxDidLogInMethod(obj, method_id)) {
    (void)va_arg(args, jstring);
    SyncRobloxCookiesFromEngine(CurrentVM());
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (IsRobloxDidSwitchAccountMethod(obj, method_id)) {
    HandleRobloxDidSwitchAccount(obj, name);
    return;
  }
  if (IsRobloxDidLogOutMethod(obj, method_id)) {
    HandleRobloxDidLogOut(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnAppReady ||
      method == JniMethod::kGameActivityOnDidSignUp ||
      method == JniMethod::kGameActivityOnMotionEventListening ||
      method == JniMethod::kGameActivityOnLuaTextBoxChanged ||
      method == JniMethod::kGameActivityOnScreenshotReady) {
    (void)va_arg(args, jstring);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnDidLogOutReceived ||
      method == JniMethod::kGameActivityOnLuaAppDidReturn ||
      method == JniMethod::kGameActivityOnLuaTextBoxPropertyChanged ||
      method == JniMethod::kGameActivityOnRestartLuaApp ||
      method == JniMethod::kGameActivityOnScanQrCode ||
      method == JniMethod::kGameActivityHideKeyboard) {
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityShowKeyboard ||
      method == JniMethod::kGameActivitySetAppUpgradeStatus) {
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strncmp(name, "gameActivity_", 13) == 0) {
    RecordNativeHelperCallback(obj, name);
    return;
  }
  MarkCallUnhandled();
}

bool IsMemStorageCallbackMethod(jobject obj, jmethodID method_id) {
  return obj != nullptr && method_id != nullptr &&
         ObjectClassName(obj) ==
             "com/roblox/engine/jni/memstorage/Callback" &&
         MethodOf(method_id) == JniMethod::kOnItemSet &&
         std::strcmp(MethodSignature(method_id),
                     "(Ljava/lang/String;)V") == 0;
}

bool HandleMemStorageCallbackVoidMethodV(jobject obj, jmethodID method_id,
                                         va_list args) {
  if (!IsMemStorageCallbackMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchMemStorageCallback(obj, vm->GetJNIEnv(),
                                         va_arg(args, jstring));
  }
  return true;
}

bool HandleMemStorageCallbackVoidMethodA(jobject obj, jmethodID method_id,
                                         const jvalue* args) {
  if (!IsMemStorageCallbackMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchMemStorageCallback(
        obj, vm->GetJNIEnv(),
        args != nullptr ? static_cast<jstring>(args[0].l) : nullptr);
  }
  return true;
}

bool IsRobloxCookieSetHandlerMethod(jobject obj, jmethodID method_id) {
  return obj != nullptr && method_id != nullptr &&
         ObjectClassName(obj) ==
             "com/roblox/universalapp/cookie/"
             "JNICookieProtocol$OnSetCookieHandler" &&
         MethodOf(method_id) == JniMethod::kOnSetCookie &&
         std::strcmp(MethodSignature(method_id),
                     "([Ljava/lang/String;Ljava/lang/String;)V") == 0;
}

bool HandleRobloxCookieSetVoidMethodV(jobject obj, jmethodID method_id,
                                      va_list args) {
  if (!IsRobloxCookieSetHandlerMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    auto cookies = va_arg(args, jobjectArray);
    auto url = va_arg(args, jstring);
    (void)vm->DispatchRobloxCookieSet(vm->GetJNIEnv(), cookies, url);
  }
  return true;
}

bool HandleRobloxCookieSetVoidMethodA(jobject obj, jmethodID method_id,
                                      const jvalue* args) {
  if (!IsRobloxCookieSetHandlerMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxCookieSet(
        vm->GetJNIEnv(),
        args != nullptr ? static_cast<jobjectArray>(args[0].l) : nullptr,
        args != nullptr ? static_cast<jstring>(args[1].l) : nullptr);
  }
  return true;
}

jobject MakeFileObject(const char* path) {
  jobject file = MakeObjectForClass("java/io/File");
  SetStringFieldRaw(file, "path", path);
  return file;
}

jobject MakeApplicationInfoObject() {
  jobject object = SingletonObject("android/content/pm/ApplicationInfo");
  SetStringFieldRaw(object, "packageName", "com.roblox.client");
  SetStringFieldRaw(object, "sourceDir", "rbx_bin/sober_apk/base.apk");
  SetStringFieldRaw(object, "publicSourceDir", "rbx_bin/sober_apk/base.apk");
  SetStringFieldRaw(object, "nativeLibraryDir", "rbx_bin");
  SetStringFieldRaw(object, "dataDir", "/data/user/0/com.roblox.client");
  SetStringFieldRaw(object, "processName", "com.roblox.client");
  SetStringFieldRaw(object, "className", "com.roblox.client.RobloxApplication");
  SetIntFieldRaw(object, "flags", 0);
  return object;
}

jobject MakePackageInfoObject() {
  jobject object = SingletonObject("android/content/pm/PackageInfo");
  SetStringFieldRaw(object, "packageName", "com.roblox.client");
  SetObjectFieldRaw(object, "applicationInfo", MakeApplicationInfoObject());
  const char* version_name = std::getenv("MOCKTAIL_ROBLOX_VERSION");
  SetStringFieldRaw(object, "versionName",
                    version_name != nullptr ? version_name : "unknown");
  const char* version_code = std::getenv("MOCKTAIL_ROBLOX_VERSION_CODE");
  const int parsed_version_code =
      version_code != nullptr ? std::atoi(version_code) : 0;
  SetIntFieldRaw(object, "versionCode", parsed_version_code);
  return object;
}

jobject MakeDeviceStaticParamsObject() {
  jobject object =
      SingletonObject("com/roblox/engine/jni/model/DeviceStaticParams");
  auto* pseudo_object = PseudoObjectFromRef(object);
  if (pseudo_object) {
    const PlatformIdentity identity = CurrentPlatformIdentity();
    SetStringFieldRaw(object, "osVersion", "Android 13");
    SetStringFieldRaw(object, "deviceName", identity.device_name.c_str());
    const char* app_version = std::getenv("MOCKTAIL_ROBLOX_VERSION");
    SetStringFieldRaw(object, "appVersion", app_version != nullptr ? app_version : "unknown");
    SetStringFieldRaw(object, "manufacturer", identity.manufacturer.c_str());
    SetStringFieldRaw(object, "model", identity.model.c_str());
    SetStringFieldRaw(object, "brand", identity.brand.c_str());
    SetStringFieldRaw(object, "device", identity.device_code.c_str());
    SetStringFieldRaw(object, "deviceSku", identity.device_sku.c_str());
    SetStringFieldRaw(object, "appBuildVariant", "headless");
    SetStringFieldRaw(object, "socModel", identity.soc_model.c_str());
    SetStringFieldRaw(object, "soc_model", identity.soc_model.c_str());
    SetBooleanFieldRaw(object, "cpu64Bit", JNI_TRUE);
    const mocktail::runtime::DisplaySize host_display =
        mocktail::runtime::ParseDisplaySize(
            std::getenv(mocktail::runtime::kDisplaySizeEnvironment));
    SetIntFieldRaw(object, "screenWidth", host_display.width);
    SetIntFieldRaw(object, "screenHeight", host_display.height);
    SetIntFieldRaw(object, "screenDensityDpi", 160);
    SetIntFieldRaw(object, "apiVersion", 33);
    SetIntFieldRaw(object, "sdkVersion", 33);
    SetFloatFieldRaw(object, "density", 1.0f);
    SetFloatFieldRaw(object, "scaledDensity", 1.0f);
    SetFloatFieldRaw(object, "xdpi", 160.0f);
    SetFloatFieldRaw(object, "ydpi", 160.0f);
  }
  return object;
}

jobject MakePlatformSystemDialogHandlerObject() {
  jobject object = SingletonObject(
      "com/roblox/protocols/systemdialog/PlatformSystemDialogHandler");
  SetBooleanFieldRaw(object, "available", JNI_TRUE);
  SetIntFieldRaw(object, "nextDialogId", 1);
  return object;
}

jobject MakeNativeTextBoxInfoObject() {
  jobject object =
      SingletonObject("com/roblox/engine/jni/model/NativeTextBoxInfo");
  SetStringFieldRaw(object, "text", "");
  SetStringFieldRaw(object, "currentText", "");
  SetStringFieldRaw(object, "hint", "");
  SetStringFieldRaw(object, "placeholder", "");
  SetStringFieldRaw(object, "keyboardType", "text");
  SetStringFieldRaw(object, "imeOptions", "none");
  SetIntFieldRaw(object, "cursorPosition", 0);
  SetIntFieldRaw(object, "selectionStart", 0);
  SetIntFieldRaw(object, "selectionEnd", 0);
  SetIntFieldRaw(object, "maxLength", 0);
  SetBooleanFieldRaw(object, "focused", JNI_FALSE);
  SetBooleanFieldRaw(object, "isFocused", JNI_FALSE);
  SetBooleanFieldRaw(object, "multiline", JNI_FALSE);
  SetBooleanFieldRaw(object, "secureTextEntry", JNI_FALSE);
  SetBooleanFieldRaw(object, "showKeyboard", JNI_FALSE);
  SetBooleanFieldRaw(object, "editable", JNI_FALSE);
  return object;
}

jobject MakeMessageBusConnectionObject() {
  jobject object =
      SingletonObject("com/roblox/universalapp/messagebus/Connection");
  SetBooleanFieldRaw(object, "connected", JNI_TRUE);
  SetBooleanFieldRaw(object, "isConnected", JNI_TRUE);
  SetIntFieldRaw(object, "id", 1);
  SetLongFieldRaw(object, "nativePtr", 1);
  return object;
}

void EnsureAndroidObjectGraph() {
  if (g_android_graph_ready) {
    return;
  }
  g_android_graph_ready = true;

  jobject activity = SingletonObject("com/roblox/client/RobloxActivity");
  jobject main_activity =
      SingletonObject("com/roblox/client/startup/MainGameActivity");
  jobject context = SingletonObject("android/content/Context");
  jobject app_context = SingletonObject("android/app/Application");
  jobject resources = SingletonObject("android/content/res/Resources");
  jobject asset_manager = SingletonObject("android/content/res/AssetManager");
  jobject package_manager = SingletonObject("android/content/pm/PackageManager");
  jobject class_loader = SingletonObject("java/lang/ClassLoader");
  jobject shared_preferences =
      SingletonObject("android/content/SharedPreferences");
  jobject preferences_editor =
      SingletonObject("android/content/SharedPreferences$Editor");
  jobject window = SingletonObject("android/view/Window");
  jobject window_manager = SingletonObject("android/view/WindowManager");
  jobject display_manager =
      SingletonObject("android/hardware/display/DisplayManager");
  jobject display = SingletonObject("android/view/Display");
  jobject decor_view = SingletonObject("android/view/View");
  jobject root_view = SingletonObject("android/view/ViewRootImpl");
  jobject surface_view = SingletonObject("android/view/SurfaceView");
  jobject surface_holder = SingletonObject("android/view/SurfaceHolder");
  jobject surface = SingletonObject("android/view/Surface");
  jobject configuration =
      SingletonObject("android/content/res/Configuration");
  jobject display_metrics = SingletonObject("android/util/DisplayMetrics");
  jobject app_info = MakeApplicationInfoObject();
  jobject package_info = MakePackageInfoObject();
  jobject system_dialog_handler = MakePlatformSystemDialogHandlerObject();
  jobject text_box_info = MakeNativeTextBoxInfoObject();
  jobject message_bus_connection = MakeMessageBusConnectionObject();
  jobject native_helper = MakeNativeHelperObject(main_activity);

  SetObjectFieldRaw(activity, "context", context);
  SetObjectFieldRaw(activity, "baseContext", context);
  SetObjectFieldRaw(activity, "applicationContext", app_context);
  SetObjectFieldRaw(activity, "resources", resources);
  SetObjectFieldRaw(activity, "assetManager", asset_manager);
  SetObjectFieldRaw(activity, "packageManager", package_manager);
  SetObjectFieldRaw(activity, "classLoader", class_loader);
  SetObjectFieldRaw(activity, "window", window);
  SetObjectFieldRaw(activity, "windowManager", window_manager);
  SetObjectFieldRaw(activity, "display", display);
  SetObjectFieldRaw(activity, "surface", surface);
  SetObjectFieldRaw(activity, "surfaceHolder", surface_holder);
  SetStringFieldRaw(activity, "packageName", "com.roblox.client");

  SetObjectFieldRaw(main_activity, "context", context);
  SetObjectFieldRaw(main_activity, "baseContext", context);
  SetObjectFieldRaw(main_activity, "applicationContext", app_context);
  SetObjectFieldRaw(main_activity, "resources", resources);
  SetObjectFieldRaw(main_activity, "assetManager", asset_manager);
  SetObjectFieldRaw(main_activity, "packageManager", package_manager);
  SetObjectFieldRaw(main_activity, "classLoader", class_loader);
  SetObjectFieldRaw(main_activity, "window", window);
  SetObjectFieldRaw(main_activity, "windowManager", window_manager);
  SetObjectFieldRaw(main_activity, "display", display);
  SetObjectFieldRaw(main_activity, "surface", surface);
  SetObjectFieldRaw(main_activity, "surfaceHolder", surface_holder);
  SetObjectFieldRaw(main_activity, "nativeHelper", native_helper);
  SetObjectFieldRaw(main_activity, "H", native_helper);
  SetStringFieldRaw(main_activity, "packageName", "com.roblox.client");
  SetBooleanFieldRaw(main_activity, "bootstrapStarted", JNI_FALSE);

  SetObjectFieldRaw(context, "applicationContext", app_context);
  SetObjectFieldRaw(context, "resources", resources);
  SetObjectFieldRaw(context, "assetManager", asset_manager);
  SetObjectFieldRaw(context, "assets", asset_manager);
  SetObjectFieldRaw(context, "packageManager", package_manager);
  SetObjectFieldRaw(context, "classLoader", class_loader);
  SetObjectFieldRaw(context, "sharedPreferences", shared_preferences);
  SetObjectFieldRaw(context, "applicationInfo", app_info);
  SetObjectFieldRaw(context, "packageInfo", package_info);
  SetObjectFieldRaw(context, "window", window);
  SetObjectFieldRaw(context, "windowManager", window_manager);
  SetObjectFieldRaw(context, "display", display);
  SetStringFieldRaw(context, "packageName", "com.roblox.client");

  SetObjectFieldRaw(app_context, "applicationContext", app_context);
  SetObjectFieldRaw(app_context, "resources", resources);
  SetObjectFieldRaw(app_context, "assetManager", asset_manager);
  SetObjectFieldRaw(app_context, "packageManager", package_manager);
  SetObjectFieldRaw(app_context, "classLoader", class_loader);
  SetObjectFieldRaw(app_context, "applicationInfo", app_info);
  SetStringFieldRaw(app_context, "packageName", "com.roblox.client");
  SetObjectFieldRaw(app_context, "systemDialogHandler", system_dialog_handler);
  SetObjectFieldRaw(app_context, "nativeTextBoxInfo", text_box_info);
  SetObjectFieldRaw(app_context, "messageBusConnection", message_bus_connection);

  SetObjectFieldRaw(resources, "assets", asset_manager);
  SetObjectFieldRaw(resources, "assetManager", asset_manager);
  SetObjectFieldRaw(resources, "configuration", configuration);
  SetObjectFieldRaw(resources, "displayMetrics", display_metrics);

  SetObjectFieldRaw(package_manager, "applicationInfo", app_info);
  SetObjectFieldRaw(package_manager, "packageInfo", package_info);

  SetObjectFieldRaw(shared_preferences, "editor", preferences_editor);
  SetBooleanFieldRaw(preferences_editor, "committed", JNI_TRUE);

  SetObjectFieldRaw(window, "decorView", decor_view);
  SetObjectFieldRaw(window, "rootView", decor_view);
  SetObjectFieldRaw(window, "surfaceView", surface_view);
  SetObjectFieldRaw(window, "windowManager", window_manager);

  SetObjectFieldRaw(window_manager, "defaultDisplay", display);
  SetObjectFieldRaw(display_manager, "display", display);

  SetObjectFieldRaw(decor_view, "rootView", decor_view);
  SetObjectFieldRaw(decor_view, "holder", surface_holder);
  SetObjectFieldRaw(decor_view, "surface", surface);
  SetObjectFieldRaw(decor_view, "display", display);
  const mocktail::runtime::DisplaySize host_window =
      mocktail::runtime::ParseDisplaySize(
          std::getenv(mocktail::runtime::kWindowSizeEnvironment));
  SetIntFieldRaw(decor_view, "width", host_window.width);
  SetIntFieldRaw(decor_view, "height", host_window.height);

  SetObjectFieldRaw(surface_view, "holder", surface_holder);
  SetObjectFieldRaw(surface_view, "surface", surface);
  SetObjectFieldRaw(surface_view, "rootView", root_view);
  SetIntFieldRaw(surface_view, "width", host_window.width);
  SetIntFieldRaw(surface_view, "height", host_window.height);

  SetObjectFieldRaw(surface_holder, "surface", surface);
  SetObjectFieldRaw(surface_holder, "surfaceFrame",
                    SingletonObject("android/graphics/Rect"));
  SetBooleanFieldRaw(surface, "valid", JNI_TRUE);
  SetBooleanFieldRaw(surface, "isValid", JNI_TRUE);

  const mocktail::runtime::DisplaySize host_display =
      mocktail::runtime::ParseDisplaySize(
          std::getenv(mocktail::runtime::kDisplaySizeEnvironment));
  SetIntFieldRaw(display, "width", host_display.width);
  SetIntFieldRaw(display, "height", host_display.height);
  SetIntFieldRaw(display, "rotation", 0);
  SetIntFieldRaw(display_metrics, "widthPixels", host_display.width);
  SetIntFieldRaw(display_metrics, "heightPixels", host_display.height);
  SetIntFieldRaw(display_metrics, "densityDpi", 160);
  SetFloatFieldRaw(display_metrics, "density", 1.0f);
  SetFloatFieldRaw(display_metrics, "scaledDensity", 1.0f);
  SetFloatFieldRaw(display_metrics, "xdpi", 160.0f);
  SetFloatFieldRaw(display_metrics, "ydpi", 160.0f);
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  SetIntFieldRaw(configuration, "orientation", 2);
  SetIntFieldRaw(configuration, "densityDpi", 160);
  // Density is 1.0, so dp equals window pixels.
  SetIntFieldRaw(configuration, "screenWidthDp", host_window.width);
  SetIntFieldRaw(configuration, "screenHeightDp", host_window.height);
  SetIntFieldRaw(configuration, "smallestScreenWidthDp",
                 std::min(host_window.width, host_window.height));
  SetIntFieldRaw(configuration, "touchscreen", identity.touch_enabled ? 3 : 1);
  SetIntFieldRaw(configuration, "keyboard", identity.keyboard_enabled ? 2 : 1);
  SetIntFieldRaw(configuration, "keyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
  SetIntFieldRaw(configuration, "hardKeyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
}

jobject SystemServiceObject(const std::string& service_name) {
  EnsureAndroidObjectGraph();
  if (service_name == "window") {
    return SingletonObject("android/view/WindowManager");
  }
  if (service_name == "display") {
    return SingletonObject("android/hardware/display/DisplayManager");
  }
  if (service_name == "audio") {
    return SingletonObject("android/media/AudioManager");
  }
  if (service_name == "input_method") {
    return SingletonObject("android/view/inputmethod/InputMethodManager");
  }
  if (service_name == "sensor") {
    return SingletonObject("android/hardware/SensorManager");
  }
  if (service_name == "connectivity") {
    return SingletonObject("android/net/ConnectivityManager");
  }
  if (service_name == "power") {
    return SingletonObject("android/os/PowerManager");
  }
  return SingletonObject("java/lang/Object");
}

jobject AndroidObjectForMethod(jmethodID method_id) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  EnsureAndroidObjectGraph();
  if (!name) {
    return nullptr;
  }
  if (method == JniMethod::kCurrentActivity ||
      method == JniMethod::kGetActivity) {
    return SingletonObject("com/roblox/client/startup/MainGameActivity");
  }
  if (method == JniMethod::kGetApplicationContext ||
      method == JniMethod::kGetBaseContext ||
      method == JniMethod::kGetContext) {
    return SingletonObject("android/content/Context");
  }
  if (method == JniMethod::kGetAssets) {
    return SingletonObject("android/content/res/AssetManager");
  }
  if (method == JniMethod::kGetAssetManager) {
    return SingletonObject("android/content/res/AssetManager");
  }
  if (method == JniMethod::kGetResources) {
    return SingletonObject("android/content/res/Resources");
  }
  if (method == JniMethod::kGetClassLoader) {
    return SingletonObject("java/lang/ClassLoader");
  }
  if (method == JniMethod::kGetSharedPreferences) {
    return SingletonObject("android/content/SharedPreferences");
  }
  if (method == JniMethod::kEdit) {
    return SingletonObject("android/content/SharedPreferences$Editor");
  }
  if (method == JniMethod::kGetPackageManager) {
    return SingletonObject("android/content/pm/PackageManager");
  }
  if (method == JniMethod::kGetApplicationInfo) {
    return MakeApplicationInfoObject();
  }
  if (method == JniMethod::kGetDeviceStaticParams) {
    return MakeDeviceStaticParamsObject();
  }
  if (method == JniMethod::kGetPlatformSystemDialogHandler ||
      method == JniMethod::kGetSystemDialogHandler) {
    return MakePlatformSystemDialogHandlerObject();
  }
  if (method == JniMethod::kGetImplementation) {
    return EngineJavaCallbackObject();
  }
  if (method == JniMethod::kGetTextBoxInfo ||
      method == JniMethod::kGetNativeTextBoxInfo ||
      method == JniMethod::kGetCurrentTextBoxInfo ||
      method == JniMethod::kNativeGetTextBoxInfo) {
    return MakeNativeTextBoxInfoObject();
  }
  if (method == JniMethod::kGetMessageBus ||
      method == JniMethod::kGetMessageBusConnection ||
      method == JniMethod::kConnect ||
      method == JniMethod::kSubscribe ||
      method == JniMethod::kSetRequestHandler ||
      method == JniMethod::kSetRequestHandlerRaw ||
      method == JniMethod::kDoSubscribeProtocolMethodResponseRaw ||
      method == JniMethod::kSubscribeProtocolMethodResponseRaw) {
    return MakeMessageBusConnectionObject();
  }
  if (method == JniMethod::kGetPackageInfo) {
    return MakePackageInfoObject();
  }
  if (method == JniMethod::kGetWindow) {
    return SingletonObject("android/view/Window");
  }
  if (method == JniMethod::kGetWindowManager ||
      method == JniMethod::kGetSystemService) {
    return SingletonObject("android/view/WindowManager");
  }
  if (method == JniMethod::kGetDefaultDisplay ||
      method == JniMethod::kGetDisplay) {
    return SingletonObject("android/view/Display");
  }
  if (method == JniMethod::kGetDecorView ||
      method == JniMethod::kGetRootView) {
    return SingletonObject("android/view/View");
  }
  if (method == JniMethod::kGetHolder) {
    return SingletonObject("android/view/SurfaceHolder");
  }
  if (method == JniMethod::kGetSurface) {
    return SingletonObject("android/view/Surface");
  }
  if (method == JniMethod::kGetFilesDir) {
    return MakeFileObject("/data/user/0/com.roblox.client/files");
  }
  if (method == JniMethod::kGetCacheDir) {
    return MakeFileObject("/data/user/0/com.roblox.client/cache");
  }
  if (method == JniMethod::kGetExternalFilesDir) {
    return MakeFileObject(
        "/sdcard/Android/data/com.roblox.client/files");
  }
  if (method == JniMethod::kLoadClass ||
      method == JniMethod::kFindClass) {
    return SingletonObject("java/lang/Class");
  }
  return nullptr;
}

jobject ObjectResultForReceiverMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name) {
    return nullptr;
  }
  EnsureAndroidObjectGraph();
  if (method == JniMethod::kGetBaseUrl ||
      method == JniMethod::kGetBaseURL ||
      method == JniMethod::kGetWwwBaseUrl ||
      method == JniMethod::kGetWWWBaseUrl) {
    return MakeString("https://www.roblox.com");
  }
  if (method == JniMethod::kGetApiBaseUrl ||
      method == JniMethod::kGetApiGatewayUrl) {
    return MakeString("https://apis.roblox.com");
  }
  if (method == JniMethod::kGetSettingsUrl ||
      method == JniMethod::kGetClientSettingsUrl) {
    return MakeString(
        "https://clientsettingscdn.roblox.com/v2/settings-compressed/"
        "application/AndroidApp.zst");
  }
  if (method == JniMethod::kGetApplicationContext) {
    jobject value = ObjectFieldValue(obj, "applicationContext");
    return value ? value : SingletonObject("android/app/Application");
  }
  if (method == JniMethod::kGetNativeHelper) {
    jobject value = ObjectFieldValue(obj, "nativeHelper");
    if (!value) {
      value = ObjectFieldValue(obj, "H");
    }
    if (!value) {
      value = MakeNativeHelperObject(obj);
      SetObjectFieldRaw(obj, "nativeHelper", value);
      SetObjectFieldRaw(obj, "H", value);
    } else if (obj) {
      SetObjectFieldRaw(value, "activity", obj);
      SetObjectFieldRaw(value, "a", obj);
    }
    return value;
  }
  if (method == JniMethod::kGetBaseContext ||
      method == JniMethod::kGetContext) {
    jobject value = ObjectFieldValue(obj, "baseContext");
    return value ? value : SingletonObject("android/content/Context");
  }
  if (method == JniMethod::kGetAssets) {
    jobject value = ObjectFieldValue(obj, "assetManager");
    return value ? value : SingletonObject("android/content/res/AssetManager");
  }
  if (method == JniMethod::kGetResources) {
    jobject value = ObjectFieldValue(obj, "resources");
    return value ? value : SingletonObject("android/content/res/Resources");
  }
  if (method == JniMethod::kGetClassLoader) {
    jobject value = ObjectFieldValue(obj, "classLoader");
    return value ? value : SingletonObject("java/lang/ClassLoader");
  }
  if (method == JniMethod::kGetPackageManager) {
    jobject value = ObjectFieldValue(obj, "packageManager");
    return value ? value : SingletonObject("android/content/pm/PackageManager");
  }
  if (method == JniMethod::kGetApplicationInfo) {
    jobject value = ObjectFieldValue(obj, "applicationInfo");
    return value ? value : MakeApplicationInfoObject();
  }
  if (method == JniMethod::kGetPackageInfo) {
    jobject value = ObjectFieldValue(obj, "packageInfo");
    return value ? value : MakePackageInfoObject();
  }
  if (method == JniMethod::kGetWindow) {
    jobject value = ObjectFieldValue(obj, "window");
    return value ? value : SingletonObject("android/view/Window");
  }
  if (method == JniMethod::kGetWindowManager) {
    jobject value = ObjectFieldValue(obj, "windowManager");
    return value ? value : SingletonObject("android/view/WindowManager");
  }
  if (method == JniMethod::kGetDefaultDisplay) {
    jobject value = ObjectFieldValue(obj, "defaultDisplay");
    return value ? value : SingletonObject("android/view/Display");
  }
  if (method == JniMethod::kGetDisplay) {
    jobject value = ObjectFieldValue(obj, "display");
    return value ? value : SingletonObject("android/view/Display");
  }
  if (method == JniMethod::kGetDecorView ||
      method == JniMethod::kGetRootView) {
    jobject value = ObjectFieldValue(obj, "decorView");
    if (!value) {
      value = ObjectFieldValue(obj, "rootView");
    }
    return value ? value : SingletonObject("android/view/View");
  }
  if (method == JniMethod::kGetHolder) {
    jobject value = ObjectFieldValue(obj, "holder");
    return value ? value : SingletonObject("android/view/SurfaceHolder");
  }
  if (method == JniMethod::kGetSurface) {
    jobject value = ObjectFieldValue(obj, "surface");
    return value ? value : SingletonObject("android/view/Surface");
  }
  if (method == JniMethod::kEdit) {
    jobject value = ObjectFieldValue(obj, "editor");
    return value ? value
                 : SingletonObject("android/content/SharedPreferences$Editor");
  }
  if (jobject value = ObjectFieldValue(obj, name)) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  if (!field_name.empty()) {
    if (jobject value = ObjectFieldValue(obj, field_name.c_str())) {
      return value;
    }
  }
  return nullptr;
}

jobject ClassObjectForName(const std::string& requested_name) {
  std::string class_name = requested_name;
  std::replace(class_name.begin(), class_name.end(), '.', '/');
  auto cls = FallbackClassForName(class_name.empty() ? "java/lang/Object"
                                                     : class_name);
  return reinterpret_cast<jobject>(StoreClass(std::move(cls)));
}

jobject ObjectResultForMethodV(jobject obj, jmethodID method_id, va_list args) {
  const JniMethod method = MethodOf(method_id);
  if (IsJavaStringGetBytesMethod(obj, method_id)) {
    return JavaStringGetUtf8Bytes(obj, va_arg(args, jstring));
  }
  if (method == JniMethod::kRun &&
      ObjectClassName(obj) ==
          "com/roblox/universalapp/messagebus/RequestHandlerRaw") {
    VM* vm = CurrentVM();
    return vm != nullptr ? vm->DispatchMessageBusRequestHandler(
                               obj, vm->GetJNIEnv(), va_arg(args, jstring))
                         : nullptr;
  }
  jobject local_storage_result = nullptr;
  if (LocalStorageObjectResultForMethodV(method_id, args, &local_storage_result)) {
    return local_storage_result;
  }
  jobject cookie_result = nullptr;
  if (CookieObjectResultForMethodV(method_id, args, &cookie_result)) {
    return cookie_result;
  }
  if (method == JniMethod::kGetSystemService) {
    jstring service = va_arg(args, jstring);
    return SystemServiceObject(StringFromJString(service));
  }
  if (method == JniMethod::kLoadClass ||
      method == JniMethod::kFindClass ||
      method == JniMethod::kForName) {
    jstring requested_name = va_arg(args, jstring);
    return ClassObjectForName(StringFromJString(requested_name));
  }
  if (method == JniMethod::kGetString) {
    (void)va_arg(args, jstring);
    jstring default_value = va_arg(args, jstring);
    return default_value ? default_value : MakeString("");
  }
  if (method == JniMethod::kGetProperty) {
    jstring property_name = va_arg(args, jstring);
    std::string property = StringFromJString(property_name);
    if (property == "android.media.property.OUTPUT_SAMPLE_RATE") {
      return MakeString("48000");
    }
    if (property == "android.media.property.OUTPUT_FRAMES_PER_BUFFER") {
      return MakeString("512");
    }
    return MakeString("");
  }
  if (method == JniMethod::kGet) {
    (void)va_arg(args, jint);
    return nullptr;
  }
  jobject receiver_result = ObjectResultForReceiverMethod(obj, method_id);
  if (receiver_result) {
    return receiver_result;
  }
  jobject result = ObjectResultForMethod(method_id);
  // Static calls probe this with a null receiver before their own fallback.
  if (result == nullptr && obj != nullptr) {
    MarkCallUnhandled();
  }
  return result;
}

jboolean BooleanResultForReceiverMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name) {
    return JNI_FALSE;
  }
  if (method == JniMethod::kIsValid) {
    jboolean value = BooleanFieldValue(obj, "isValid");
    return value ? value : BooleanFieldValue(obj, "valid");
  }
  if (method == JniMethod::kCommit ||
      method == JniMethod::kApply ||
      method == JniMethod::kContains ||
      method == JniMethod::kAttachObserver ||
      method == JniMethod::kDetachObserver ||
      method == JniMethod::kIsAvailable ||
      method == JniMethod::kIsConnected ||
      method == JniMethod::kIsEmpty) {
    return JNI_TRUE;
  }
  if (method == JniMethod::kIsDestroyed ||
      method == JniMethod::kIsFinishing ||
      method == JniMethod::kIsChangingConfigurations ||
      method == JniMethod::kIsBluetoothA2dpOn ||
      method == JniMethod::kIsBluetoothScoOn) {
    return JNI_FALSE;
  }
  jboolean value = BooleanFieldValue(obj, name);
  if (value) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  value = field_name.empty() ? JNI_FALSE
                             : BooleanFieldValue(obj, field_name.c_str());
  if (!value) {
    MarkCallUnhandled();
  }
  return value;
}

bool PackageManagerBooleanResultForMethodV(jobject obj, jmethodID method_id,
                                           va_list args, jboolean* result) {
  if (result == nullptr ||
      ObjectClassName(obj) != "android/content/pm/PackageManager" ||
      MethodOf(method_id) != JniMethod::kHasSystemFeature) {
    return false;
  }
  const jstring feature = va_arg(args, jstring);
  const std::string feature_name = StringFromJString(feature);
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  if (feature_name == "android.hardware.type.pc") {
    *result = identity.pc_hardware ? JNI_TRUE : JNI_FALSE;
  } else if (feature_name == "android.hardware.touchscreen" ||
             feature_name == "android.hardware.touchscreen.multitouch" ||
             feature_name ==
                 "android.hardware.touchscreen.multitouch.distinct" ||
             feature_name ==
                 "android.hardware.touchscreen.multitouch.jazzhand") {
    *result = identity.touch_enabled ? JNI_TRUE : JNI_FALSE;
  } else {
    *result = JNI_FALSE;
  }
  return true;
}

bool PackageManagerBooleanResultForMethodA(jobject obj, jmethodID method_id,
                                           const jvalue* args,
                                           jboolean* result) {
  if (result == nullptr ||
      ObjectClassName(obj) != "android/content/pm/PackageManager" ||
      MethodOf(method_id) != JniMethod::kHasSystemFeature) {
    return false;
  }
  const std::string feature_name =
      args != nullptr ? StringFromJString(reinterpret_cast<jstring>(args[0].l))
                      : std::string();
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  if (feature_name == "android.hardware.type.pc") {
    *result = identity.pc_hardware ? JNI_TRUE : JNI_FALSE;
  } else if (feature_name.rfind("android.hardware.touchscreen", 0) == 0) {
    *result = identity.touch_enabled ? JNI_TRUE : JNI_FALSE;
  } else {
    *result = JNI_FALSE;
  }
  return true;
}

jint IntResultForReceiverMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name) {
    return 0;
  }
  if (method == JniMethod::kSize ||
      method == JniMethod::kLength) {
    return IntFieldValue(obj, name);
  }
  jint value = IntFieldValue(obj, name);
  if (value != 0) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  if (!field_name.empty()) {
    value = IntFieldValue(obj, field_name.c_str());
    if (value != 0) {
      return value;
    }
  }
  return IntResultForName(method_id);
}

bool IsFmodAudioDeviceMethod(jobject obj, jmethodID method_id,
                             JniMethod method, const char* signature) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), signature) == 0 &&
         ObjectClassName(obj) == "org/fmod/AudioDevice";
}

bool IsWebRtcAudioManagerMethod(jobject obj, jmethodID method_id,
                                JniMethod method, const char* signature) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), signature) == 0 &&
         ObjectClassName(obj) == "org/webrtc/voiceengine/WebRtcAudioManager";
}

bool HandleWebRtcAudioManagerBooleanMethod(jobject obj, jmethodID method_id,
                                           jboolean* result) {
  VM* vm = CurrentVM();
  if (IsWebRtcAudioManagerMethod(obj, method_id, JniMethod::kInit, "()Z")) {
    *result = vm != nullptr && vm->DispatchWebRtcAudioManagerInit(obj)
                  ? JNI_TRUE
                  : JNI_FALSE;
  } else if (IsWebRtcAudioManagerMethod(obj, method_id,
                                        JniMethod::kIsDeviceBlacklistedForOpenSLESUsage,
                                        "()Z")) {
    // Host recording is provided by AudioRecord JNI, not Android OpenSL ES.
    *result = JNI_TRUE;
  } else if (IsWebRtcAudioManagerMethod(obj, method_id,
                                        JniMethod::kIsCommunicationModeEnabled, "()Z")) {
    *result = BooleanFieldValue(obj, "initialized");
  } else if (IsWebRtcAudioManagerMethod(obj, method_id,
                                        JniMethod::kIsLowLatencyOutputSupported, "()Z")) {
    *result = BooleanFieldValue(obj, "lowLatencyOutput");
  } else if (IsWebRtcAudioManagerMethod(obj, method_id,
                                        JniMethod::kIsLowLatencyInputSupported, "()Z")) {
    *result = BooleanFieldValue(obj, "lowLatencyInput");
  } else if (IsWebRtcAudioManagerMethod(obj, method_id, JniMethod::kIsProAudioSupported,
                                        "()Z")) {
    *result = BooleanFieldValue(obj, "proAudio");
  } else if (IsWebRtcAudioManagerMethod(obj, method_id, JniMethod::kIsAAudioSupported,
                                        "()Z")) {
    *result = BooleanFieldValue(obj, "aAudio");
  } else {
    return false;
  }
  return true;
}

bool HandleWebRtcAudioManagerVoidMethodV(jobject obj, jmethodID method_id,
                                         va_list args) {
  VM* vm = CurrentVM();
  if (IsWebRtcAudioManagerMethod(obj, method_id, JniMethod::kDispose, "()V")) {
    if (vm != nullptr) vm->DispatchWebRtcAudioManagerDispose(obj);
  } else if (IsWebRtcAudioManagerMethod(obj, method_id, JniMethod::kSetMicrophoneMute,
                                        "(Z)V")) {
    const bool muted = va_arg(args, jint) != JNI_FALSE;
    if (vm != nullptr) vm->DispatchWebRtcAudioManagerMicrophoneMute(obj, muted);
  } else {
    return false;
  }
  return true;
}

bool HandleWebRtcAudioManagerVoidMethodA(jobject obj, jmethodID method_id,
                                         const jvalue* args) {
  VM* vm = CurrentVM();
  if (IsWebRtcAudioManagerMethod(obj, method_id, JniMethod::kDispose, "()V")) {
    if (vm != nullptr) vm->DispatchWebRtcAudioManagerDispose(obj);
  } else if (IsWebRtcAudioManagerMethod(obj, method_id, JniMethod::kSetMicrophoneMute,
                                        "(Z)V")) {
    if (vm != nullptr && args != nullptr) {
      vm->DispatchWebRtcAudioManagerMicrophoneMute(obj, args[0].z != JNI_FALSE);
    }
  } else {
    return false;
  }
  return true;
}

bool IsWebRtcAudioRecordMethod(jobject obj, jmethodID method_id,
                               JniMethod method, const char *signature) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), signature) == 0 &&
         ObjectClassName(obj) == "org/webrtc/voiceengine/WebRtcAudioRecord";
}

bool IsWebRtcAudioTrackMethod(jobject obj, jmethodID method_id,
                              JniMethod method, const char *signature) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), signature) == 0 &&
         ObjectClassName(obj) == "org/webrtc/voiceengine/WebRtcAudioTrack";
}

constexpr char kRobloxNativeGlJavaInterfaceClass[] =
    "com/roblox/engine/jni/NativeGLJavaInterface";
constexpr char kRobloxEngineJavaCallbackClass[] =
    "com/roblox/engine/jni/EngineJavaCallback2";
constexpr char kRobloxNativeHelperClass[] =
    "com/roblox/client/startup/NativeHelper";
constexpr char kRobloxShowKeyboardSignature[] =
    "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V";
constexpr char kRobloxLegacyTextBoxInfoConstructor[] = "(FFFFFZIIIIIIZZ)V";
constexpr char kRobloxTextBoxInfoConstructor[] = "(FFFFFZIIIIIIZZZ)V";

bool IsRobloxTextBoxInfoConstructor(jmethodID method_id) {
  if (MethodOf(method_id) != JniMethod::kConstructor) {
    return false;
  }
  const char* signature = MethodSignature(method_id);
  return std::strcmp(signature, kRobloxLegacyTextBoxInfoConstructor) == 0 ||
         std::strcmp(signature, kRobloxTextBoxInfoConstructor) == 0;
}

bool HasRobloxTextBoxEditableArgument(jmethodID method_id) {
  return std::strcmp(MethodSignature(method_id),
                     kRobloxTextBoxInfoConstructor) == 0;
}

void ClearSensitiveString(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = '\0';
  }
  value->clear();
}

bool IsValidUtf8(const std::vector<jbyte>& bytes) {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const auto first = static_cast<std::uint8_t>(bytes[index]);
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7f) {
      ++index;
      continue;
    }
    if ((first & 0xe0) == 0xc0) {
      continuation_count = 1;
      code_point = first & 0x1f;
      if (code_point < 2) {
        return false;
      }
    } else if ((first & 0xf0) == 0xe0) {
      continuation_count = 2;
      code_point = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      continuation_count = 3;
      code_point = first & 0x07;
    } else {
      return false;
    }
    if (continuation_count > bytes.size() - index - 1) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto next = static_cast<std::uint8_t>(bytes[index + offset]);
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

bool SnapshotRobloxTextInputShow(jlong text_box, jboolean show_native_input,
                                 jbyteArray text_array, jobject info_object,
                                 RobloxTextInputShowRequest* request) {
  if (text_box <= 0 || text_array == nullptr || info_object == nullptr ||
      request == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoArray* array = ArrayFromRef(text_array);
  auto* info = PseudoObjectFromRef(info_object);
  if (array == nullptr || info == nullptr ||
      ObjectClassName(info_object) !=
          "com/roblox/engine/jni/model/NativeTextBoxInfo" ||
      array->bytes.size() > 32768 || !IsValidUtf8(array->bytes)) {
    return false;
  }

  RobloxTextInputShowRequest snapshot;
  snapshot.text_box = static_cast<std::int64_t>(text_box);
  snapshot.show_native_input = show_native_input == JNI_TRUE;
  if (!array->bytes.empty()) {
    snapshot.text.assign(reinterpret_cast<const char*>(array->bytes.data()),
                         array->bytes.size());
  }
  snapshot.info.x = FloatFieldValue(info_object, "x");
  snapshot.info.y = FloatFieldValue(info_object, "y");
  snapshot.info.width = FloatFieldValue(info_object, "width");
  snapshot.info.height = FloatFieldValue(info_object, "height");
  snapshot.info.font_size = FloatFieldValue(info_object, "fontSize");
  snapshot.info.multiline = BooleanFieldValue(info_object, "multiline");
  snapshot.info.x_alignment = IntFieldValue(info_object, "xAlignment");
  snapshot.info.y_alignment = IntFieldValue(info_object, "yAlignment");
  snapshot.info.text_color = IntFieldValue(info_object, "textColor");
  snapshot.info.font = IntFieldValue(info_object, "font");
  snapshot.info.text_input_type = IntFieldValue(info_object, "textInputType");
  snapshot.info.return_key_type = IntFieldValue(info_object, "returnKeyType");
  snapshot.info.manual_focus_release =
      BooleanFieldValue(info_object, "manualFocusRelease");
  snapshot.info.text_wrapped = BooleanFieldValue(info_object, "textWrapped");
  *request = std::move(snapshot);
  return true;
}

bool IsRobloxTextInputStaticMethod(jclass clazz, jmethodID method_id,
                                   JniMethod method,
                                   const char* signature) {
  const auto cls = ClassFromJClass(clazz);
  return cls != nullptr &&
         cls->GetName() == kRobloxNativeGlJavaInterfaceClass &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), signature) == 0;
}

bool IsRobloxTextInputInstanceMethod(jobject obj, jmethodID method_id,
                                     JniMethod method,
                                     const char* signature) {
  return obj != nullptr &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), signature) == 0 &&
         ObjectClassName(obj) == kRobloxNativeHelperClass;
}

bool IsRobloxEngineJavaTextInputMethod(jobject obj, jmethodID method_id,
                                       JniMethod method,
                                       const char* signature) {
  return obj != nullptr && method_id != nullptr &&
         MethodOf(method_id) == method &&
         std::strcmp(MethodSignature(method_id), signature) == 0 &&
         ObjectClassName(obj) == kRobloxEngineJavaCallbackClass;
}

bool HandleRobloxExperienceLifecycleVoidMethod(jobject obj,
                                                jmethodID method_id) {
  if (obj == nullptr || method_id == nullptr ||
      MethodOf(method_id) != JniMethod::kGameActivityOnLuaAppDidReturn ||
      std::strcmp(MethodSignature(method_id), "()V") != 0 ||
      ObjectClassName(obj) != kRobloxNativeHelperClass) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxExperienceLuaAppDidReturn();
  }
  return true;
}

bool HandleRobloxExperienceLifecycleStaticVoidMethod(jclass clazz,
                                                      jmethodID method_id) {
  const std::shared_ptr<Class> method_class = ClassFromJClass(clazz);
  if (method_class == nullptr || method_id == nullptr ||
      method_class->GetName() != kRobloxNativeGlJavaInterfaceClass ||
      MethodOf(method_id) != JniMethod::kGameDidLeave ||
      std::strcmp(MethodSignature(method_id), "()V") != 0) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxExperienceLuaAppDidReturn();
  }
  return true;
}

bool IsRobloxPromptNativePurchase(jclass clazz, jmethodID method_id) {
  const std::shared_ptr<Class> method_class = ClassFromJClass(clazz);
  if (method_class == nullptr || method_id == nullptr ||
      method_class->GetName() != kRobloxNativeGlJavaInterfaceClass) {
    return false;
  }
  const JniMethod method = MethodOf(method_id);
  const std::string_view signature(MethodSignature(method_id));
  return (method == JniMethod::kPromptNativePurchase ||
          method == JniMethod::kPromptNativePurchaseWithPayload ||
          method == JniMethod::kPromptNativePurchaseWithPaymentSessionId) &&
         signature.rfind("(JLjava/lang/String;", 0) == 0 &&
         signature.size() >= 2 &&
         signature.substr(signature.size() - 2) == ")V";
}

void DispatchRobloxPromptNativePurchase(jmethodID method_id, jlong player_id,
                                        jstring product_id) {
  std::string product;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    product = StringFromJString(product_id);
  }
  std::cerr << "  [store] " << MethodName(method_id)
            << " requested; Google Play is not available\n";
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    vm->DispatchRobloxStorePurchase(player_id, product);
  }
}

bool HandleRobloxPromptNativePurchaseV(jclass clazz, jmethodID method_id,
                                       va_list args) {
  if (!IsRobloxPromptNativePurchase(clazz, method_id)) {
    return false;
  }
  const jlong player_id = va_arg(args, jlong);
  const auto product_id = va_arg(args, jstring);
  DispatchRobloxPromptNativePurchase(method_id, player_id, product_id);
  return true;
}

bool HandleRobloxPromptNativePurchaseA(jclass clazz, jmethodID method_id,
                                       const jvalue* args) {
  if (!IsRobloxPromptNativePurchase(clazz, method_id)) {
    return false;
  }
  if (args != nullptr) {
    DispatchRobloxPromptNativePurchase(method_id, args[0].j,
                                       static_cast<jstring>(args[1].l));
  }
  return true;
}

bool DispatchRobloxTextInputShow(jlong text_box, jboolean show_native_input,
                                 jbyteArray text, jobject info) {
  RobloxTextInputShowRequest request;
  if (!SnapshotRobloxTextInputShow(text_box, show_native_input, text, info,
                                   &request)) {
    return true;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxTextInputShow(request);
  }
  ClearSensitiveString(&request.text);
  return true;
}

void DispatchRobloxTextInputReplaceText(jstring text) {
  if (text == nullptr) {
    return;
  }
  std::string snapshot;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    snapshot = StringFromJString(text);
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxTextInputReplaceText(snapshot);
  }
  ClearSensitiveString(&snapshot);
}

bool HandleRobloxTextInputStaticVoidMethodV(jclass clazz,
                                             jmethodID method_id,
                                             va_list args) {
  if (IsRobloxTextInputStaticMethod(clazz, method_id, JniMethod::kShowKeyboard,
                                    kRobloxShowKeyboardSignature)) {
    const jlong text_box = va_arg(args, jlong);
    const auto show_native_input =
        static_cast<jboolean>(va_arg(args, jint));
    const auto text = va_arg(args, jbyteArray);
    const auto info = va_arg(args, jobject);
    return DispatchRobloxTextInputShow(text_box, show_native_input, text, info);
  }
  if (IsRobloxTextInputStaticMethod(clazz, method_id, JniMethod::kHideKeyboard, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, JniMethod::kOnLuaTextBoxChangedCallback,
          "(Ljava/lang/String;)V")) {
    DispatchRobloxTextInputReplaceText(va_arg(args, jstring));
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, JniMethod::kOnLuaTextBoxPropertyChangedCallback, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool HandleRobloxTextInputStaticVoidMethodA(jclass clazz,
                                             jmethodID method_id,
                                             const jvalue* args) {
  if (IsRobloxTextInputStaticMethod(clazz, method_id, JniMethod::kShowKeyboard,
                                    kRobloxShowKeyboardSignature)) {
    if (args != nullptr) {
      return DispatchRobloxTextInputShow(
          args[0].j, args[1].z, static_cast<jbyteArray>(args[2].l), args[3].l);
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(clazz, method_id, JniMethod::kHideKeyboard, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, JniMethod::kOnLuaTextBoxChangedCallback,
          "(Ljava/lang/String;)V")) {
    if (args != nullptr) {
      DispatchRobloxTextInputReplaceText(static_cast<jstring>(args[0].l));
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, JniMethod::kOnLuaTextBoxPropertyChangedCallback, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool HandleRobloxTextInputInstanceVoidMethodV(jobject obj,
                                               jmethodID method_id,
                                               va_list args) {
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      JniMethod::kGameActivityShowKeyboard,
                                      kRobloxShowKeyboardSignature) ||
      IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kQ,
                                        kRobloxShowKeyboardSignature)) {
    const jlong text_box = va_arg(args, jlong);
    const auto show_native_input =
        static_cast<jboolean>(va_arg(args, jint));
    const auto text = va_arg(args, jbyteArray);
    const auto info = va_arg(args, jobject);
    return DispatchRobloxTextInputShow(text_box, show_native_input, text, info);
  }
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      JniMethod::kGameActivityHideKeyboard, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kD, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, JniMethod::kGameActivityOnLuaTextBoxChanged,
          "(Ljava/lang/String;)V")) {
    DispatchRobloxTextInputReplaceText(va_arg(args, jstring));
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kG,
                                        "(Ljava/lang/String;)V")) {
    DispatchRobloxTextInputReplaceText(va_arg(args, jstring));
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, JniMethod::kGameActivityOnLuaTextBoxPropertyChanged, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kH, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool HandleRobloxTextInputInstanceVoidMethodA(jobject obj,
                                               jmethodID method_id,
                                               const jvalue* args) {
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      JniMethod::kGameActivityShowKeyboard,
                                      kRobloxShowKeyboardSignature) ||
      IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kQ,
                                        kRobloxShowKeyboardSignature)) {
    if (args != nullptr) {
      return DispatchRobloxTextInputShow(
          args[0].j, args[1].z, static_cast<jbyteArray>(args[2].l), args[3].l);
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      JniMethod::kGameActivityHideKeyboard, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kD, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, JniMethod::kGameActivityOnLuaTextBoxChanged,
          "(Ljava/lang/String;)V")) {
    if (args != nullptr) {
      DispatchRobloxTextInputReplaceText(static_cast<jstring>(args[0].l));
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kG,
                                        "(Ljava/lang/String;)V")) {
    if (args != nullptr) {
      DispatchRobloxTextInputReplaceText(static_cast<jstring>(args[0].l));
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, JniMethod::kGameActivityOnLuaTextBoxPropertyChanged, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, JniMethod::kH, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool DispatchFmodAudioDeviceInit(jobject obj, jint channels,
                                 jint sample_rate_hz,
                                 jint block_size_frames, jint block_count) {
  VM* vm = CurrentVM();
  if (vm == nullptr) {
    return false;
  }
  return vm->DispatchFmodAudioDeviceInit(
      obj, channels, sample_rate_hz, block_size_frames, block_count);
}

bool HandleFmodAudioDeviceBooleanMethodV(jobject obj, jmethodID method_id,
                                         va_list args, jboolean* result) {
  if (!IsFmodAudioDeviceMethod(obj, method_id, JniMethod::kInit, "(IIII)Z")) {
    return false;
  }
  const jint channels = va_arg(args, jint);
  const jint sample_rate_hz = va_arg(args, jint);
  const jint block_size_frames = va_arg(args, jint);
  const jint block_count = va_arg(args, jint);
  const bool initialized = DispatchFmodAudioDeviceInit(
      obj, channels, sample_rate_hz, block_size_frames, block_count);
  if (!initialized) {
    std::cerr << "  [mocktail][audio] FMOD AudioDevice.init rejected by "
                 "host bridge\n";
  }
  if (result != nullptr) {
    *result = initialized ? JNI_TRUE : JNI_FALSE;
  }
  return true;
}

bool HandleFmodAudioDeviceBooleanMethodA(jobject obj, jmethodID method_id,
                                         const jvalue* args,
                                         jboolean* result) {
  if (!IsFmodAudioDeviceMethod(obj, method_id, JniMethod::kInit, "(IIII)Z")) {
    return false;
  }
  const bool initialized =
      args != nullptr &&
      DispatchFmodAudioDeviceInit(obj, args[0].i, args[1].i, args[2].i,
                                  args[3].i);
  if (!initialized) {
    std::cerr << "  [mocktail][audio] FMOD AudioDevice.init rejected by "
                 "host bridge\n";
  }
  if (result != nullptr) {
    *result = initialized ? JNI_TRUE : JNI_FALSE;
  }
  return true;
}

bool HandleWebRtcAudioRecordIntMethodV(jobject obj, jmethodID method_id,
                                       va_list args, jint *result) {
  if (!IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kInitRecording, "(II)I")) {
    return false;
  }
  const jint sample_rate_hz = va_arg(args, jint);
  const jint channels = va_arg(args, jint);
  void *direct_buffer = nullptr;
  std::size_t direct_buffer_capacity = 0;
  VM *vm = CurrentVM();
  const int frames = vm != nullptr
                         ? vm->DispatchWebRtcAudioRecordInit(
                               obj, sample_rate_hz, channels, &direct_buffer,
                               &direct_buffer_capacity)
                         : -1;
  if (result != nullptr) {
    *result = frames;
  }
  return true;
}

bool HandleWebRtcAudioRecordIntMethodA(jobject obj, jmethodID method_id,
                                       const jvalue *args, jint *result) {
  if (!IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kInitRecording, "(II)I")) {
    return false;
  }
  void *direct_buffer = nullptr;
  std::size_t direct_buffer_capacity = 0;
  VM *vm = CurrentVM();
  const int frames = vm != nullptr && args != nullptr
                         ? vm->DispatchWebRtcAudioRecordInit(
                               obj, args[0].i, args[1].i, &direct_buffer,
                               &direct_buffer_capacity)
                         : -1;
  if (result != nullptr) {
    *result = frames;
  }
  return true;
}

bool HandleWebRtcAudioRecordBooleanMethodV(jobject obj, jmethodID method_id,
                                           va_list args, jboolean *result) {
  bool handled = true;
  bool value = false;
  VM *vm = CurrentVM();
  if (IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kStartRecording, "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioRecordStart(obj);
  } else if (IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kStopRecording,
                                       "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioRecordStop(obj);
  } else if (IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kEnableBuiltInAEC,
                                       "(Z)Z") ||
             IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kEnableBuiltInNS,
                                       "(Z)Z") ||
             IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kEnableBuiltInAGC,
                                       "(Z)Z")) {
    (void)va_arg(args, jint);
    // WebRTC's software processing remains active; these Android controls are
    // accepted as compatibility no-ops.
    value = true;
  } else if (IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kIsAudioConfigVerified,
                                       "()Z") ||
             IsWebRtcAudioRecordMethod(obj, method_id,
                                       JniMethod::kIsAudioSourceMatchingRecordingSession,
                                       "()Z")) {
    value = true;
  } else {
    handled = false;
  }
  if (handled && result != nullptr) {
    *result = value ? JNI_TRUE : JNI_FALSE;
  }
  return handled;
}

bool HandleWebRtcAudioRecordBooleanMethodA(jobject obj, jmethodID method_id,
                                           const jvalue * /*args*/,
                                           jboolean *result) {
  bool handled = true;
  bool value = false;
  VM *vm = CurrentVM();
  if (IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kStartRecording, "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioRecordStart(obj);
  } else if (IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kStopRecording,
                                       "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioRecordStop(obj);
  } else if (IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kEnableBuiltInAEC,
                                       "(Z)Z") ||
             IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kEnableBuiltInNS,
                                       "(Z)Z") ||
             IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kEnableBuiltInAGC,
                                       "(Z)Z") ||
             IsWebRtcAudioRecordMethod(obj, method_id, JniMethod::kIsAudioConfigVerified,
                                       "()Z") ||
             IsWebRtcAudioRecordMethod(obj, method_id,
                                       JniMethod::kIsAudioSourceMatchingRecordingSession,
                                       "()Z")) {
    value = true;
  } else {
    handled = false;
  }
  if (handled && result != nullptr) {
    *result = value ? JNI_TRUE : JNI_FALSE;
  }
  return handled;
}

bool HandleWebRtcAudioTrackIntMethodV(jobject obj, jmethodID method_id,
                                      va_list args, jint *result) {
  VM *vm = CurrentVM();
  jint value = 0;
  if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kInitPlayout, "(IID)I")) {
    const jint sample_rate_hz = va_arg(args, jint);
    const jint channels = va_arg(args, jint);
    const jdouble buffer_size_factor = va_arg(args, jdouble);
    void *direct_buffer = nullptr;
    std::size_t direct_buffer_capacity = 0;
    value = vm != nullptr
                ? vm->DispatchWebRtcAudioTrackInit(
                      obj, sample_rate_hz, channels, buffer_size_factor,
                      &direct_buffer, &direct_buffer_capacity)
                : -1;
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kGetBufferSizeInFrames,
                                      "()I")) {
    value =
        vm != nullptr ? vm->DispatchWebRtcAudioTrackBufferSizeFrames(obj) : 0;
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kGetStreamMaxVolume,
                                      "()I") ||
             IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kGetStreamVolume,
                                      "()I")) {
    // Host volume remains under SDL/the desktop mixer. Return a stable,
    // internally consistent Android-style range to WebRTC.
    value = 100;
  } else {
    return false;
  }
  if (result != nullptr) {
    *result = value;
  }
  return true;
}

bool HandleWebRtcAudioTrackIntMethodA(jobject obj, jmethodID method_id,
                                      const jvalue *args, jint *result) {
  VM *vm = CurrentVM();
  jint value = 0;
  if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kInitPlayout, "(IID)I")) {
    void *direct_buffer = nullptr;
    std::size_t direct_buffer_capacity = 0;
    value = vm != nullptr && args != nullptr
                ? vm->DispatchWebRtcAudioTrackInit(obj, args[0].i, args[1].i,
                                                   args[2].d, &direct_buffer,
                                                   &direct_buffer_capacity)
                : -1;
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kGetBufferSizeInFrames,
                                      "()I")) {
    value =
        vm != nullptr ? vm->DispatchWebRtcAudioTrackBufferSizeFrames(obj) : 0;
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kGetStreamMaxVolume,
                                      "()I") ||
             IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kGetStreamVolume,
                                      "()I")) {
    value = 100;
  } else {
    return false;
  }
  if (result != nullptr) {
    *result = value;
  }
  return true;
}

bool HandleWebRtcAudioTrackBooleanMethodV(jobject obj, jmethodID method_id,
                                          va_list args, jboolean *result) {
  VM *vm = CurrentVM();
  bool value = false;
  if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kStartPlayout, "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioTrackStart(obj);
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kStopPlayout, "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioTrackStop(obj);
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kSetStreamVolume,
                                      "(I)Z")) {
    (void)va_arg(args, jint);
    value = true;
  } else {
    return false;
  }
  if (result != nullptr) {
    *result = value ? JNI_TRUE : JNI_FALSE;
  }
  return true;
}

bool HandleWebRtcAudioTrackBooleanMethodA(jobject obj, jmethodID method_id,
                                          const jvalue * /*args*/,
                                          jboolean *result) {
  VM *vm = CurrentVM();
  bool value = false;
  if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kStartPlayout, "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioTrackStart(obj);
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kStopPlayout, "()Z")) {
    value = vm != nullptr && vm->DispatchWebRtcAudioTrackStop(obj);
  } else if (IsWebRtcAudioTrackMethod(obj, method_id, JniMethod::kSetStreamVolume,
                                      "(I)Z")) {
    value = true;
  } else {
    return false;
  }
  if (result != nullptr) {
    *result = value ? JNI_TRUE : JNI_FALSE;
  }
  return true;
}

bool DispatchFmodAudioDeviceWrite(jobject obj, jbyteArray array,
                                  jint requested_size) {
  if (requested_size <= 0) {
    return false;
  }
  const std::size_t size = static_cast<std::size_t>(requested_size);
  // Reused per audio thread; the write callback copies the bytes it keeps.
  thread_local std::vector<std::uint8_t> snapshot;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    PseudoArray* stored = ArrayFromRef(array);
    if (stored == nullptr || size > stored->bytes.size()) {
      return false;
    }
    if (snapshot.size() < size) {
      snapshot.resize(size);
    }
    std::memcpy(snapshot.data(), stored->bytes.data(), size);
  }
  VM* vm = CurrentVM();
  if (vm == nullptr) {
    return false;
  }
  return vm->DispatchFmodAudioDeviceWrite(obj, snapshot.data(), size);
}

bool HandleFmodAudioDeviceVoidMethodV(jobject obj, jmethodID method_id,
                                      va_list args) {
  if (IsFmodAudioDeviceMethod(obj, method_id, JniMethod::kWrite, "([BI)V")) {
    const auto array = va_arg(args, jbyteArray);
    const jint size = va_arg(args, jint);
    (void)DispatchFmodAudioDeviceWrite(obj, array, size);
    return true;
  }
  if (IsFmodAudioDeviceMethod(obj, method_id, JniMethod::kClose, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchFmodAudioDeviceClose(obj);
    }
    return true;
  }
  return false;
}

bool HandleFmodAudioDeviceVoidMethodA(jobject obj, jmethodID method_id,
                                      const jvalue* args) {
  if (IsFmodAudioDeviceMethod(obj, method_id, JniMethod::kWrite, "([BI)V")) {
    const auto array =
        args == nullptr ? nullptr : static_cast<jbyteArray>(args[0].l);
    const jint size = args == nullptr ? -1 : args[1].i;
    (void)DispatchFmodAudioDeviceWrite(obj, array, size);
    return true;
  }
  if (IsFmodAudioDeviceMethod(obj, method_id, JniMethod::kClose, "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchFmodAudioDeviceClose(obj);
    }
    return true;
  }
  return false;
}

// NativeQuoteInterface.requestResponse([B)[B reply when no Android Keystore
// exists: status byte 1, attestation-available byte 0, then the error text.
// A real device with a keystore answers [1, 1, chain] instead.
jbyteArray MakeQuoteUnavailableResponse() {
  static const char kMessage[] =
      "java.security.KeyStoreException: AndroidKeyStore is not available";
  const std::size_t message_length = sizeof(kMessage) - 1;
  jbyteArray result = MakeByteArray(static_cast<jsize>(2 + message_length));
  PseudoArray* array = ArrayFromRef(result);
  if (array == nullptr || array->bytes.size() != 2 + message_length) {
    return nullptr;
  }
  array->bytes[0] = 1;
  array->bytes[1] = 0;
  std::memcpy(array->bytes.data() + 2, kMessage, message_length);
  return result;
}

jobject StaticObjectResultForMethod(jmethodID method_id) {
  const JniMethod method = MethodOf(method_id);
  if (method == JniMethod::kRequestResponse &&
      std::strcmp(MethodSignature(method_id), "([B)[B") == 0) {
    return MakeQuoteUnavailableResponse();
  }
  const PlatformIdentity identity = CurrentPlatformIdentity();
  jobject android_object = AndroidObjectForMethod(method_id);
  if (android_object) {
    return android_object;
  }
  if (method == JniMethod::kGetLocale ||
      method == JniMethod::kGetRobloxLocale ||
      method == JniMethod::kGetGameLocale) {
    return MakeString("en_us");
  }
  if (method == JniMethod::kGetUsername ||
      method == JniMethod::kGetLastLoggedInUser) {
    return MakeString(RobloxIdentityForJava().username.c_str());
  }
  if (method == JniMethod::kGetDisplayName ||
      method == JniMethod::kGetAlternateName) {
    return MakeString(RobloxIdentityForJava().display_name.c_str());
  }
  if (method == JniMethod::kGetPlatformName) {
    return MakeString(identity.platform_name.c_str());
  }
  if (method == JniMethod::kGetBaseUrl ||
      method == JniMethod::kGetBaseURL ||
      method == JniMethod::kGetWwwBaseUrl ||
      method == JniMethod::kGetWWWBaseUrl) {
    return MakeString("https://www.roblox.com");
  }
  if (method == JniMethod::kGetApiBaseUrl ||
      method == JniMethod::kGetApiGatewayUrl) {
    return MakeString("https://apis.roblox.com");
  }
  if (method == JniMethod::kGetSettingsUrl ||
      method == JniMethod::kGetClientSettingsUrl) {
    return MakeString(
        "https://clientsettingscdn.roblox.com/v2/settings-compressed/"
        "application/AndroidApp.zst");
  }
  if (method == JniMethod::kGetDevice) {
    return MakeString(identity.device_code.c_str());
  }
  if (method == JniMethod::kGetBrand) {
    return MakeString(identity.brand.c_str());
  }
  if (method == JniMethod::kGetDeviceModel ||
      method == JniMethod::kGetModel) {
    return MakeString(identity.model.c_str());
  }
  if (method == JniMethod::kGetProduct) {
    return MakeString(identity.device_code.c_str());
  }
  if (method == JniMethod::kGetBuildId) {
    return MakeString("MOCKTAIL");
  }
  if (method == JniMethod::kGetBuildType) {
    return MakeString("user");
  }
  if (method == JniMethod::kGetBuildRelease) {
    return MakeString("13");
  }
  if (method == JniMethod::kGetDeviceManufacturer ||
      method == JniMethod::kGetManufacturer) {
    return MakeString(identity.manufacturer.c_str());
  }
  if (method == JniMethod::kGetPackageName) {
    return MakeString("com.roblox.client");
  }
  if (method == JniMethod::kGetInstallerPackageName) {
    return MakeString("com.android.vending");
  }
  if (method == JniMethod::kGetAbsolutePath ||
      method == JniMethod::kGetCanonicalPath ||
      method == JniMethod::kGetPath) {
    return MakeString("/data/user/0/com.roblox.client/files");
  }
  if (method == JniMethod::kGetString ||
      method == JniMethod::kOptString) {
    return MakeString("");
  }
  if (method == JniMethod::kGetProperty) {
    return MakeString("");
  }
  if (method == JniMethod::kGetTheme) {
    return MakeString(ResolvedDarkTheme() ? "Dark" : "Light");
  }
  if (method == JniMethod::kGetFilesDir) {
    return MakeString("/data/user/0/com.roblox.client/files");
  }
  if (method == JniMethod::kGetAppVersion) {
    const char* app_version = std::getenv("MOCKTAIL_ROBLOX_VERSION");
    return MakeString(app_version != nullptr ? app_version : "unknown");
  }
  if (method == JniMethod::kGetLastLoggedInUserId) {
    return MakeString(std::to_string(RobloxIdentityForJava().user_id).c_str());
  }
  if (method == JniMethod::kGetPublicIPv4Addresseses) {
    return MakeString("127.0.0.1");
  }
  if (method == JniMethod::kGetVideoCodecs) {
    return MakeObjectArray(0, nullptr);
  }
  if (method == JniMethod::kGetAssetManager) {
    return SingletonObject("android/content/res/AssetManager");
  }
  if (method == JniMethod::kGetPlatformSystemDialogHandler ||
      method == JniMethod::kGetSystemDialogHandler) {
    return MakePlatformSystemDialogHandlerObject();
  }
  if (method == JniMethod::kGetTextBoxInfo ||
      method == JniMethod::kGetNativeTextBoxInfo ||
      method == JniMethod::kGetCurrentTextBoxInfo ||
      method == JniMethod::kNativeGetTextBoxInfo) {
    return MakeNativeTextBoxInfoObject();
  }
  if (method == JniMethod::kGetText ||
      method == JniMethod::kGetCurrentText ||
      method == JniMethod::kGetPlaceholder ||
      method == JniMethod::kGetHint ||
      method == JniMethod::kGetLastRaw) {
    return MakeString("");
  }
  if (method == JniMethod::kGetMessageBus ||
      method == JniMethod::kGetMessageBusConnection ||
      method == JniMethod::kConnect ||
      method == JniMethod::kSubscribe ||
      method == JniMethod::kSetRequestHandler ||
      method == JniMethod::kSetRequestHandlerRaw ||
      method == JniMethod::kDoSubscribeProtocolMethodResponseRaw ||
      method == JniMethod::kSubscribeProtocolMethodResponseRaw) {
    return MakeMessageBusConnectionObject();
  }
  MarkCallUnhandled();
  return nullptr;
}

jobject ObjectResultForMethod(jmethodID method_id) {
  const JniMethod method = MethodOf(method_id);
  const PlatformIdentity identity = CurrentPlatformIdentity();
  jobject android_object = AndroidObjectForMethod(method_id);
  if (android_object) {
    return android_object;
  }
  if (method == JniMethod::kGetPackageName) {
    return MakeString("com.roblox.client");
  }
  if (method == JniMethod::kGetDevice) {
    return MakeString(identity.device_code.c_str());
  }
  if (method == JniMethod::kGetBrand) {
    return MakeString(identity.brand.c_str());
  }
  if (method == JniMethod::kGetDeviceModel ||
      method == JniMethod::kGetModel) {
    return MakeString(identity.model.c_str());
  }
  if (method == JniMethod::kGetProduct) {
    return MakeString(identity.device_code.c_str());
  }
  if (method == JniMethod::kGetBuildId) {
    return MakeString("MOCKTAIL");
  }
  if (method == JniMethod::kGetBuildType) {
    return MakeString("user");
  }
  if (method == JniMethod::kGetBuildRelease) {
    return MakeString("13");
  }
  if (method == JniMethod::kGetDeviceManufacturer ||
      method == JniMethod::kGetManufacturer) {
    return MakeString(identity.manufacturer.c_str());
  }
  if (method == JniMethod::kGetInstallerPackageName) {
    return MakeString("com.android.vending");
  }
  if (method == JniMethod::kGetAbsolutePath ||
      method == JniMethod::kGetCanonicalPath ||
      method == JniMethod::kGetPath) {
    return MakeString("/data/user/0/com.roblox.client/files");
  }
  if (method == JniMethod::kGetString ||
      method == JniMethod::kOptString) {
    return MakeString("");
  }
  if (method == JniMethod::kGetProperty) {
    return MakeString("");
  }
  return nullptr;
}

void DispatchAppBridgeNotification(JNIEnv* env, jstring type, jstring data) {
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxAppBridgeNotification(env, type, data);
  }
  jobject listener = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    listener = StaticObjectFieldValue("sAppBridgeNotificationListener");
  }
  if (!listener || !env) {
    DeleteLocalJniReference(listener);
    return;
  }
  jclass listener_class = env->GetObjectClass(listener);
  jmethodID method = env->GetMethodID(
      listener_class, "a", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (method) {
    env->CallVoidMethod(listener, method, type, data);
  }
  env->DeleteLocalRef(listener_class);
  DeleteLocalJniReference(listener);
}

void DispatchDataModelNotification(JNIEnv* env, jstring type, jstring data) {
  jobject callback = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    callback = StaticObjectFieldValue("sImplementation");
  }
  if (callback == nullptr) {
    callback = EngineJavaCallbackObject();
  }
  if (callback == nullptr) {
    return;
  }
  if (env == nullptr) {
    RecordDataModelNotification(callback, type, data);
    DeleteLocalJniReference(callback);
    return;
  }
  jclass callback_class = env->GetObjectClass(callback);
  jmethodID method = env->GetMethodID(
      callback_class, "f", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (method != nullptr) {
    env->CallVoidMethod(callback, method, type, data);
  } else {
    RecordDataModelNotification(callback, type, data);
  }
  env->DeleteLocalRef(callback_class);
  DeleteLocalJniReference(callback);
}

void HandleStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID methodID,
                             va_list args) {
  if (HandleRobloxTextInputStaticVoidMethodV(clazz, methodID, args)) {
    return;
  }
  if (HandleRobloxPromptNativePurchaseV(clazz, methodID, args)) {
    return;
  }
  if (HandleRobloxExperienceLifecycleStaticVoidMethod(clazz, methodID)) {
    return;
  }
  const char *name = MethodName(methodID);
  const JniMethod method = MethodOf(methodID);
  if (!name) {
    return;
  }
  const std::shared_ptr<Class> method_class = ClassFromJClass(clazz);
  if (method == JniMethod::kOpenNativeOverlay &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    jstring title = va_arg(args, jstring);
    jstring url = va_arg(args, jstring);
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxNativeOverlay(env, title, url);
    }
    return;
  }
  if (method == JniMethod::kInit) {
    (void)va_arg(args, jobject);
    g_fmod_initialized = true;
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD.init shimmed initialized=true\n";
    }
    return;
  }
  if (method == JniMethod::kClose ||
      method == JniMethod::kOutputAAudioHeadphonesChanged) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD." << name << " shimmed no-op\n";
    }
    return;
  }
  if (method == JniMethod::kSetAppBridgeNotificationListener) {
    jobject listener = va_arg(args, jobject);
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      SetStaticObjectFieldRaw("sAppBridgeNotificationListener", listener);
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface listener=" << listener
                << '\n';
    }
    return;
  }
  if (method == JniMethod::kSetImplementation) {
    jobject callback = va_arg(args, jobject);
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      SetStaticObjectFieldRaw("sImplementation", callback);
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface implementation=" << callback
                << '\n';
    }
    return;
  }
  if (method == JniMethod::kOnAppBridgeNotification) {
    jstring type = va_arg(args, jstring);
    jstring data = va_arg(args, jstring);
    std::cout << "  [JNI callback] onAppBridgeNotification type="
              << StringFromJString(type)
              << " data_bytes=" << StringFromJString(data).size() << '\n';
    DispatchAppBridgeNotification(env, type, data);
    return;
  }
  if (method == JniMethod::kOnDataModelNotificationCallback &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    jstring type = va_arg(args, jstring);
    jstring data = va_arg(args, jstring);
    DispatchDataModelNotification(env, type, data);
    return;
  }
  MarkCallUnhandled();
}

void HandleStaticVoidMethodA(JNIEnv *env, jclass clazz, jmethodID methodID,
                             const jvalue *args) {
  if (HandleRobloxTextInputStaticVoidMethodA(clazz, methodID, args)) {
    return;
  }
  if (HandleRobloxPromptNativePurchaseA(clazz, methodID, args)) {
    return;
  }
  if (HandleRobloxExperienceLifecycleStaticVoidMethod(clazz, methodID)) {
    return;
  }
  const char *name = MethodName(methodID);
  const JniMethod method = MethodOf(methodID);
  if (!name) {
    return;
  }
  if (method == JniMethod::kInit) {
    g_fmod_initialized = true;
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD.init shimmed initialized=true\n";
    }
    return;
  }
  if (method == JniMethod::kClose ||
      method == JniMethod::kOutputAAudioHeadphonesChanged) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD." << name << " shimmed no-op\n";
    }
    return;
  }
  if (!args) {
    return;
  }
  const std::shared_ptr<Class> method_class = ClassFromJClass(clazz);
  if (method == JniMethod::kOpenNativeOverlay &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxNativeOverlay(env,
                                            static_cast<jstring>(args[0].l),
                                            static_cast<jstring>(args[1].l));
    }
    return;
  }
  if (method == JniMethod::kSetAppBridgeNotificationListener) {
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      SetStaticObjectFieldRaw("sAppBridgeNotificationListener", args[0].l);
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface listener=" << args[0].l
                << '\n';
    }
    return;
  }
  if (method == JniMethod::kSetImplementation) {
    jobject callback = args[0].l;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      SetStaticObjectFieldRaw("sImplementation", callback);
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface implementation=" << callback
                << '\n';
    }
    return;
  }
  if (method == JniMethod::kOnAppBridgeNotification) {
    std::cout << "  [JNI callback] onAppBridgeNotification type="
              << StringFromJString(static_cast<jstring>(args[0].l))
              << " data_bytes="
              << StringFromJString(static_cast<jstring>(args[1].l)).size()
              << '\n';
    DispatchAppBridgeNotification(env, static_cast<jstring>(args[0].l),
                                  static_cast<jstring>(args[1].l));
    return;
  }
  if (method == JniMethod::kOnDataModelNotificationCallback &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    DispatchDataModelNotification(env, static_cast<jstring>(args[0].l),
                                  static_cast<jstring>(args[1].l));
    return;
  }
  MarkCallUnhandled();
}

bool IsRobloxOpenWebActivityMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  const char* signature = MethodSignature(method_id);
  return obj != nullptr && name != nullptr && signature != nullptr &&
         ObjectClassName(obj) == "com/roblox/client/startup/MainGameActivity" &&
         method == JniMethod::kOpenWebActivity &&
         std::strcmp(signature, "(Ljava/lang/String;Ljava/lang/String;)V") == 0;
}

bool HandleRobloxOpenWebActivityMethodV(jobject obj, jmethodID method_id,
                                        va_list args) {
  if (!IsRobloxOpenWebActivityMethod(obj, method_id)) {
    return false;
  }
  jstring url = va_arg(args, jstring);
  jstring title = va_arg(args, jstring);
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxOpenWebActivity(vm->GetJNIEnv(), url, title);
  }
  return true;
}

bool HandleRobloxOpenWebActivityMethodA(jobject obj, jmethodID method_id,
                                        const jvalue* args) {
  if (!IsRobloxOpenWebActivityMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr && args != nullptr) {
    (void)vm->DispatchRobloxOpenWebActivity(vm->GetJNIEnv(),
                                            static_cast<jstring>(args[0].l),
                                            static_cast<jstring>(args[1].l));
  }
  return true;
}

bool IsAndroidSetWindowFlagsMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  const char* signature = MethodSignature(method_id);
  const std::string_view class_name = ObjectClassName(obj);
  return obj != nullptr && name != nullptr && signature != nullptr &&
         (class_name == "android/app/Activity" ||
          class_name == "com/roblox/client/RobloxActivity" ||
          class_name == "com/roblox/client/startup/MainGameActivity" ||
          class_name == "com/google/androidgamesdk/GameActivity") &&
         method == JniMethod::kSetWindowFlags &&
         std::strcmp(signature, "(II)V") == 0;
}

bool HandleAndroidSetWindowFlagsMethodV(jobject obj, jmethodID method_id,
                                        va_list args) {
  if (!IsAndroidSetWindowFlagsMethod(obj, method_id)) {
    return false;
  }
  const jint flags = va_arg(args, jint);
  const jint mask = va_arg(args, jint);
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchAndroidWindowFlags(flags, mask);
  }
  return true;
}

bool HandleAndroidSetWindowFlagsMethodA(jobject obj, jmethodID method_id,
                                        const jvalue* args) {
  if (!IsAndroidSetWindowFlagsMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr && args != nullptr) {
    (void)vm->DispatchAndroidWindowFlags(args[0].i, args[1].i);
  }
  return true;
}

void HandleVoidMethodA(jobject obj, jmethodID method_id, const jvalue *args) {
  const char *name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name) {
    return;
  }
  if (HandleAndroidSetWindowFlagsMethodA(obj, method_id, args)) {
    return;
  }
  if (IsRobloxCookieSyncMethod(obj, method_id)) {
    SyncRobloxCookiesFromEngine(CurrentVM());
    return;
  }
  if (IsRobloxDidSwitchAccountMethod(obj, method_id)) {
    HandleRobloxDidSwitchAccount(obj, name);
    return;
  }
  if (IsRobloxDidLogOutMethod(obj, method_id)) {
    HandleRobloxDidLogOut(obj, name);
    return;
  }
  if (!args) {
    return;
  }
  const std::string_view class_name = ObjectClassName(obj);
  if (method == JniMethod::kRun &&
      class_name ==
          "com/roblox/universalapp/messagebus/RequestHandlerAsyncRaw" &&
      std::strcmp(MethodSignature(method_id),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      vm->DispatchMessageBusAsyncRequestHandler(
          obj, vm->GetJNIEnv(), static_cast<jstring>(args[0].l),
          static_cast<jstring>(args[1].l));
    }
    return;
  }
  if (method == JniMethod::kRun &&
      class_name == "com/roblox/universalapp/messagebus/RawCallback") {
    VM *vm = CurrentVM();
    if (vm != nullptr) {
      vm->DispatchMessageBusRawCallback(obj, vm->GetJNIEnv(),
                                        static_cast<jstring>(args[0].l));
    }
    return;
  }
  if (method == JniMethod::kA &&
      class_name == "com/roblox/engine/jni/OnAppBridgeNotificationListener") {
    RecordAppBridgeNotification(obj, static_cast<jstring>(args[0].l),
                                static_cast<jstring>(args[1].l));
    return;
  }
  if (method == JniMethod::kF &&
      std::strcmp(MethodSignature(method_id),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      class_name == "com/roblox/engine/jni/EngineJavaCallback2") {
    RecordDataModelNotification(obj, static_cast<jstring>(args[0].l),
                                static_cast<jstring>(args[1].l));
    return;
  }
  if (class_name == "com/roblox/engine/jni/EngineJavaCallback2" &&
      std::strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'o') {
    return;
  }
  if (method == JniMethod::kSetBaseUrl) {
    std::string base_url =
        StringFromJString(reinterpret_cast<jstring>(args[0].l));
    if (base_url.empty()) {
      base_url = "https://www.roblox.com";
    }
    SetStringFieldRaw(obj, "baseUrl", base_url.c_str());
    return;
  }
  if (method == JniMethod::kSetCookie) {
    SetStringFieldRaw(obj, "cookieName",
                      StringFromJString(
                          reinterpret_cast<jstring>(args[0].l)).c_str());
    SetStringFieldRaw(obj, "cookieValue",
                      StringFromJString(
                          reinterpret_cast<jstring>(args[1].l)).c_str());
    StoreCookieHeader(
        StringFromJString(reinterpret_cast<jstring>(args[1].l)));
    return;
  }
  if (method == JniMethod::kL0) {
    SetIntFieldRaw(obj, "contentViewId", args[0].i);
    SetIntFieldRaw(obj, "m", args[0].i);
    return;
  }
  if (method == JniMethod::kK1) {
    SetNativeHelperBoolean(MakeNativeHelperObject(nullptr), "isVisible", "g",
                           args[0].z);
    return;
  }
  if (method == JniMethod::kGameActivityOnEngineInitialized) {
    SetNativeHelperBoolean(obj, "isEngineInitialized", "i", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnFlagsLoaded) {
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_TRUE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnFlagsFailed) {
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_FALSE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnGameLoaded) {
    SetLongFieldRaw(obj, "placeId", args[0].j);
    SetNativeHelperBoolean(obj, "isInExperience", "j",
                           args[0].j > 0 ? JNI_TRUE : JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnExperienceStart) {
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnExperienceStop) {
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (method == JniMethod::kGameActivityOnScreenOrientationChanged) {
    SetIntFieldRaw(obj, "orientation", args[0].i);
    SetNativeHelperBoolean(obj, "isInExperience", "j", args[1].z);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (IsRobloxDidLogInMethod(obj, method_id)) {
    SyncRobloxCookiesFromEngine(CurrentVM());
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strncmp(name, "gameActivity_", 13) == 0) {
    RecordNativeHelperCallback(obj, name);
    return;
  }
  MarkCallUnhandled();
}

void JNICALL CallStaticVoidMethod(JNIEnv* env, jclass clazz,
                                  jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticVoidMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(clazz, methodID, true);
  va_list args;
  va_start(args, methodID);
  HandleStaticVoidMethodV(env, clazz, methodID, args);
  va_end(args);
}

jobject JNICALL CallStaticObjectMethod(JNIEnv * /*env*/, jclass clazz,
                                       jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticObjectMethod: " << MethodName(methodID)
              << '\n';
  }
  const UnhandledCallScope unhandled_call(clazz, methodID, true);
  va_list args;
  va_start(args, methodID);
  jobject result = ObjectResultForMethodV(nullptr, methodID, args);
  va_end(args);
  if (result != nullptr) {
    return result;
  }
  result = ExactMessageBusStaticObject(clazz, methodID);
  return result != nullptr ? result : StaticObjectResultForMethod(methodID);
}

jboolean StaticBooleanResultForMethod(jmethodID method_id) {
  if (MethodOf(method_id) == JniMethod::kIsSystemThemeAvailable) {
    return JNI_TRUE;
  }
  jboolean result = JNI_FALSE;
  if (FmodBooleanResultForMethod(method_id, &result)) {
    return result;
  }
  if (CookieBooleanResultForMethod(method_id, &result)) {
    return result;
  }
  MarkCallUnhandled();
  return JNI_FALSE;
}

jboolean JNICALL CallStaticBooleanMethod(JNIEnv* /*env*/, jclass clazz, jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticBooleanMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(clazz, methodID, true);
  return StaticBooleanResultForMethod(methodID);
}

jlong LongResultForReceiverMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const JniMethod method = MethodOf(method_id);
  if (!name) {
    return 0;
  }
  if (method == JniMethod::kOpen) {
    jint id = IntFieldValue(obj, "nextDialogId");
    id = id <= 0 ? 1 : id;
    SetIntFieldRaw(obj, "nextDialogId", id + 1);
    return static_cast<jlong>(id);
  }
  if (method == JniMethod::kGetId) {
    return static_cast<jlong>(IntFieldValue(obj, "id"));
  }
  jlong value = LongFieldValue(obj, name);
  if (value != 0) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  if (!field_name.empty()) {
    value = LongFieldValue(obj, field_name.c_str());
    if (value != 0) {
      return value;
    }
  }
  MarkCallUnhandled();
  return 0;
}

jint JNICALL CallStaticIntMethod(JNIEnv * /*env*/, jclass clazz,
                                 jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticIntMethod: " << MethodName(methodID)
              << '\n';
  }
  const UnhandledCallScope unhandled_call(clazz, methodID, true);
  va_list args;
  va_start(args, methodID);
  jint result = StaticIntResultForMethodV(methodID, args);
  va_end(args);
  return result;
}

jlong JNICALL CallStaticLongMethod(JNIEnv * /*env*/, jclass clazz,
                                   jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticLongMethod: " << MethodName(methodID)
              << '\n';
  }
  const UnhandledCallScope unhandled_call(clazz, methodID, true);
  return StaticLongResultForMethod(methodID);
}

void JNICALL CallVoidMethod(JNIEnv * /*env*/, jobject obj, jmethodID methodID,
                            ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallVoidMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(obj, methodID, false);
  va_list args;
  va_start(args, methodID);
  if (!HandleRobloxExperienceLifecycleVoidMethod(obj, methodID) &&
      !HandleWebRtcAudioManagerVoidMethodV(obj, methodID, args) &&
      !HandleRobloxOpenWebActivityMethodV(obj, methodID, args) &&
      !HandleRobloxTextInputInstanceVoidMethodV(obj, methodID, args) &&
      !HandleFmodAudioDeviceVoidMethodV(obj, methodID, args) &&
      !HandleRobloxCookieSetVoidMethodV(obj, methodID, args) &&
      !HandleMemStorageCallbackVoidMethodV(obj, methodID, args)) {
    HandleVoidMethod(obj, methodID, args);
  }
  va_end(args);
}

jobject JNICALL CallObjectMethod(JNIEnv* /*env*/, jobject obj,
                                 jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallObjectMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(obj, methodID, false);
  va_list args;
  va_start(args, methodID);
  jobject result = ObjectResultForMethodV(obj, methodID, args);
  va_end(args);
  return result;
}

jboolean JNICALL CallBooleanMethod(JNIEnv* /*env*/, jobject obj,
                                   jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallBooleanMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(obj, methodID, false);
  va_list args;
  va_start(args, methodID);
  jboolean result = JNI_FALSE;
  bool handled = HandleWebRtcAudioManagerBooleanMethod(obj, methodID, &result);
  if (!handled) {
    handled =
        HandleWebRtcAudioRecordBooleanMethodV(obj, methodID, args, &result);
  }
  if (!handled) {
    handled =
        HandleWebRtcAudioTrackBooleanMethodV(obj, methodID, args, &result);
  }
  if (!handled) {
    handled = HandleFmodAudioDeviceBooleanMethodV(obj, methodID, args, &result);
  }
  if (!handled) {
    handled =
        PackageManagerBooleanResultForMethodV(obj, methodID, args, &result);
  }
  if (!handled) {
    handled =
        LocalStorageBooleanResultForMethodV(methodID, args, &result);
  }
  if (!handled) {
    handled = CookieBooleanResultForMethod(methodID, &result);
  }
  va_end(args);
  return handled ? result
                 : BooleanResultForReceiverMethod(obj, methodID);
}

jint JNICALL CallIntMethod(JNIEnv* /*env*/, jobject obj, jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallIntMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(obj, methodID, false);
  va_list args;
  va_start(args, methodID);
  jint result = 0;
  bool handled =
      HandleWebRtcAudioRecordIntMethodV(obj, methodID, args, &result);
  if (!handled) {
    handled = HandleWebRtcAudioTrackIntMethodV(obj, methodID, args, &result);
  }
  va_end(args);
  return handled ? result
                 : IntResultForReceiverMethod(obj, methodID);
}

jlong JNICALL CallLongMethod(JNIEnv* /*env*/, jobject obj,
                             jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallLongMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(obj, methodID, false);
  jlong result = 0;
  if (LocalStorageLongResultForMethod(methodID, &result)) {
    return result;
  }
  return LongResultForReceiverMethod(obj, methodID);
}

jobject ConstructObjectV(jclass clazz, jmethodID methodID, va_list args) {
  jobject object = MakeObject(clazz);
  const std::shared_ptr<Class> object_class = ClassFromJClass(clazz);
  if (object_class == nullptr) {
    return object;
  }
  if (object_class->GetName() ==
          "com/roblox/universalapp/messagebus/Connection" &&
      MethodOf(methodID) == JniMethod::kConstructor &&
      std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    const jlong handle = va_arg(args, jlong);
    SetLongFieldRaw(object, "a", handle);
    SetLongFieldRaw(object, "f10205a", handle);
    SetLongFieldRaw(object, "nativePtr", handle);
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/memstorage/Connection" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "ref", va_arg(args, jlong));
  } else if (object_class->GetName() ==
                 "org/webrtc/voiceengine/WebRtcAudioManager" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    const jlong native_audio_manager = va_arg(args, jlong);
    if (VM* vm = CurrentVM()) {
      vm->DispatchWebRtcAudioManagerConstruct(object, native_audio_manager);
    }
  } else if (object_class->GetName() ==
                 "org/webrtc/voiceengine/WebRtcAudioRecord" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "nativeAudioRecord", va_arg(args, jlong));
  } else if (object_class->GetName() ==
                 "org/webrtc/voiceengine/WebRtcAudioTrack" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "nativeAudioTrack", va_arg(args, jlong));
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             IsRobloxTextBoxInfoConstructor(methodID)) {
    SetFloatFieldRaw(object, "x", static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "y", static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "width",
                     static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "height",
                     static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "fontSize",
                     static_cast<jfloat>(va_arg(args, double)));
    SetBooleanFieldRaw(object, "multiline",
                       static_cast<jboolean>(va_arg(args, jint)));
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (const char *field : kIntFields) {
      SetIntFieldRaw(object, field, va_arg(args, jint));
    }
    SetBooleanFieldRaw(object, "manualFocusRelease",
                       static_cast<jboolean>(va_arg(args, jint)));
    SetBooleanFieldRaw(object, "textWrapped",
                       static_cast<jboolean>(va_arg(args, jint)));
    if (HasRobloxTextBoxEditableArgument(methodID)) {
      SetBooleanFieldRaw(object, "editable",
                         static_cast<jboolean>(va_arg(args, jint)));
    }
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(
                 MethodSignature(methodID),
                 "(Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)V") == 0) {
    const jobject source = va_arg(args, jobject);
    constexpr const char *kFloatFields[] = {"x", "y", "width", "height",
                                            "fontSize"};
    for (const char *field : kFloatFields) {
      SetFloatFieldRaw(object, field, FloatFieldValue(source, field));
    }
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (const char *field : kIntFields) {
      SetIntFieldRaw(object, field, IntFieldValue(source, field));
    }
    constexpr const char* kBooleanFields[] = {"multiline", "manualFocusRelease",
                                              "textWrapped", "editable"};
    for (const char *field : kBooleanFields) {
      SetBooleanFieldRaw(object, field, BooleanFieldValue(source, field));
    }
  }
  return object;
}

jobject ConstructObjectA(jclass clazz, jmethodID methodID, const jvalue *args) {
  jobject object = MakeObject(clazz);
  const std::shared_ptr<Class> object_class = ClassFromJClass(clazz);
  if (object_class == nullptr || args == nullptr) {
    return object;
  }
  if (object_class->GetName() ==
          "com/roblox/universalapp/messagebus/Connection" &&
      MethodOf(methodID) == JniMethod::kConstructor &&
      std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "a", args[0].j);
    SetLongFieldRaw(object, "f10205a", args[0].j);
    SetLongFieldRaw(object, "nativePtr", args[0].j);
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/memstorage/Connection" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "ref", args[0].j);
  } else if (object_class->GetName() ==
                 "org/webrtc/voiceengine/WebRtcAudioManager" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    if (VM* vm = CurrentVM()) {
      vm->DispatchWebRtcAudioManagerConstruct(object, args[0].j);
    }
  } else if (object_class->GetName() ==
                 "org/webrtc/voiceengine/WebRtcAudioRecord" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "nativeAudioRecord", args[0].j);
  } else if (object_class->GetName() ==
                 "org/webrtc/voiceengine/WebRtcAudioTrack" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "nativeAudioTrack", args[0].j);
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             IsRobloxTextBoxInfoConstructor(methodID)) {
    constexpr const char *kFloatFields[] = {"x", "y", "width", "height",
                                            "fontSize"};
    for (std::size_t index = 0; index < 5; ++index) {
      SetFloatFieldRaw(object, kFloatFields[index], args[index].f);
    }
    SetBooleanFieldRaw(object, "multiline", args[5].z);
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (std::size_t index = 0; index < 6; ++index) {
      SetIntFieldRaw(object, kIntFields[index], args[index + 6].i);
    }
    SetBooleanFieldRaw(object, "manualFocusRelease", args[12].z);
    SetBooleanFieldRaw(object, "textWrapped", args[13].z);
    if (HasRobloxTextBoxEditableArgument(methodID)) {
      SetBooleanFieldRaw(object, "editable", args[14].z);
    }
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             MethodOf(methodID) == JniMethod::kConstructor &&
             std::strcmp(
                 MethodSignature(methodID),
                 "(Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)V") == 0) {
    const jobject source = args[0].l;
    constexpr const char *kFloatFields[] = {"x", "y", "width", "height",
                                            "fontSize"};
    for (const char *field : kFloatFields) {
      SetFloatFieldRaw(object, field, FloatFieldValue(source, field));
    }
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (const char *field : kIntFields) {
      SetIntFieldRaw(object, field, IntFieldValue(source, field));
    }
    constexpr const char* kBooleanFields[] = {"multiline", "manualFocusRelease",
                                              "textWrapped", "editable"};
    for (const char *field : kBooleanFields) {
      SetBooleanFieldRaw(object, field, BooleanFieldValue(source, field));
    }
  }
  return object;
}

jobject JNICALL NewObject(JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
                          ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] NewObject: " << MethodName(methodID) << '\n';
  }
  va_list args;
  va_start(args, methodID);
  jobject object = ConstructObjectV(clazz, methodID, args);
  va_end(args);
  return object;
}

jbyte JNICALL CallStaticByteMethod(JNIEnv* /*env*/, jclass clazz,
                                   jmethodID methodID, ...) {
  ReportUnhandledCall(clazz, methodID, true);
  return 0;
}

jchar JNICALL CallStaticCharMethod(JNIEnv* /*env*/, jclass clazz,
                                   jmethodID methodID, ...) {
  ReportUnhandledCall(clazz, methodID, true);
  return 0;
}

jshort JNICALL CallStaticShortMethod(JNIEnv* /*env*/, jclass clazz,
                                     jmethodID methodID, ...) {
  ReportUnhandledCall(clazz, methodID, true);
  return 0;
}

jfloat JNICALL CallStaticFloatMethod(JNIEnv* /*env*/, jclass clazz,
                                     jmethodID methodID, ...) {
  ReportUnhandledCall(clazz, methodID, true);
  return 0.0f;
}

jdouble JNICALL CallStaticDoubleMethod(JNIEnv* /*env*/, jclass clazz,
                                       jmethodID methodID, ...) {
  ReportUnhandledCall(clazz, methodID, true);
  return 0.0;
}

jbyte JNICALL CallByteMethod(JNIEnv* /*env*/, jobject obj,
                             jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallByteMethod: " << MethodName(methodID) << '\n';
  }
  ReportUnhandledCall(obj, methodID, false);
  return 0;
}

jchar JNICALL CallCharMethod(JNIEnv* /*env*/, jobject obj,
                             jmethodID methodID, ...) {
  ReportUnhandledCall(obj, methodID, false);
  return 0;
}

jshort JNICALL CallShortMethod(JNIEnv* /*env*/, jobject obj,
                               jmethodID methodID, ...) {
  ReportUnhandledCall(obj, methodID, false);
  return 0;
}

jfloat FloatResultForReceiverMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  if (!name) {
    return 0.0f;
  }
  jfloat value = FloatFieldValue(obj, name);
  if (value != 0.0f) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  value = field_name.empty() ? 0.0f
                             : FloatFieldValue(obj, field_name.c_str());
  if (value == 0.0f) {
    MarkCallUnhandled();
  }
  return value;
}

jfloat JNICALL CallFloatMethod(JNIEnv* /*env*/, jobject obj,
                               jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallFloatMethod: " << MethodName(methodID) << '\n';
  }
  const UnhandledCallScope unhandled_call(obj, methodID, false);
  return FloatResultForReceiverMethod(obj, methodID);
}

jdouble JNICALL CallDoubleMethod(JNIEnv* /*env*/, jobject obj,
                                 jmethodID methodID, ...) {
  ReportUnhandledCall(obj, methodID, false);
  return 0.0;
}
}  // namespace

jobject CreateAndroidConfiguration(JNIEnv* env) {
  if (env == nullptr) {
    return nullptr;
  }
  jclass clazz = env->FindClass("android/content/res/Configuration");
  if (clazz == nullptr) {
    return nullptr;
  }
  jobject configuration = env->AllocObject(clazz);
  env->DeleteLocalRef(clazz);
  if (configuration == nullptr) {
    return nullptr;
  }
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  SetIntFieldRaw(configuration, "colorMode", 0);
  SetIntFieldRaw(configuration, "densityDpi", 160);
  SetIntFieldRaw(configuration, "fontWeightAdjustment", 0);
  SetIntFieldRaw(configuration, "hardKeyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
  SetIntFieldRaw(configuration, "keyboard", identity.keyboard_enabled ? 2 : 1);
  SetIntFieldRaw(configuration, "keyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
  SetIntFieldRaw(configuration, "mcc", 0);
  SetIntFieldRaw(configuration, "mnc", 0);
  SetIntFieldRaw(configuration, "navigation", 1);
  SetIntFieldRaw(configuration, "navigationHidden", 1);
  SetIntFieldRaw(configuration, "orientation", 2);
  // Density is 1.0, so dp equals window pixels.
  const mocktail::runtime::DisplaySize config_window =
      mocktail::runtime::ParseDisplaySize(
          std::getenv(mocktail::runtime::kWindowSizeEnvironment));
  SetIntFieldRaw(configuration, "screenHeightDp", config_window.height);
  SetIntFieldRaw(configuration, "screenLayout", 0);
  SetIntFieldRaw(configuration, "screenWidthDp", config_window.width);
  SetIntFieldRaw(configuration, "smallestScreenWidthDp",
                 std::min(config_window.width, config_window.height));
  SetIntFieldRaw(configuration, "touchscreen", identity.touch_enabled ? 3 : 1);
  SetIntFieldRaw(configuration, "uiMode", 0);
  return configuration;
}

VM::VM() {
  InitJNIFunctionTables();
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  g_live_vms.push_back(this);
}

VM::~VM() {
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_live_vms.erase(std::remove(g_live_vms.begin(), g_live_vms.end(), this),
                    g_live_vms.end());
    if (g_thread_vm_instance == this) {
      g_thread_audio_fp_mode.Restore();
      g_thread_vm_instance = nullptr;
      g_thread_local_env = nullptr;
      g_thread_env_storage.functions = nullptr;
    }
  }
  {
    std::lock_guard<std::mutex> lock(message_bus_async_request_handler_mutex_);
    message_bus_async_request_handler_bindings_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    message_bus_raw_bindings_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
    message_bus_request_handler_bindings_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    mem_storage_callback_bindings_.clear();
  }
  ClearRobloxDataModelNotificationCallbacks();
  ClearRobloxExperienceLifecycleCallbacks();
  ClearRobloxCredentialSink();
  ClearRobloxCredentialProvider();
  ClearRobloxTextInputCallbacks();
  ClearAndroidWindowCallbacks();
  ClearWebRtcAudioManagerCallbacks();
  ClearWebRtcAudioTrackCallbacks();
  ClearWebRtcAudioRecordCallbacks();
  ClearFmodAudioDeviceCallbacks();
}

VM* VM::FromJavaVM(JavaVM* java_vm) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  for (VM* vm : g_live_vms) {
    if (vm->java_vm_ == java_vm) {
      return vm;
    }
  }
  return nullptr;
}

struct VM::RobloxTextInputBinding {
  ~RobloxTextInputBinding() {
    ClearSensitiveString(&last_request.text);
    if (context != nullptr && callbacks.shutdown != nullptr) {
      callbacks.shutdown(context.get());
    }
  }

  std::shared_ptr<void> context;
  RobloxTextInputCallbacks callbacks;
  std::recursive_mutex callback_mutex;
  bool active = false;
  RobloxTextInputShowRequest last_request;
};

void VM::SetRobloxTextInputCallbacks(
    std::shared_ptr<void> context,
    const RobloxTextInputCallbacks& callbacks) {
  auto binding = std::make_shared<RobloxTextInputBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  std::shared_ptr<RobloxTextInputBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    old_binding = std::move(roblox_text_input_binding_);
    roblox_text_input_binding_ = std::move(binding);
  }
}

void VM::ClearRobloxTextInputCallbacks() {
  std::shared_ptr<RobloxTextInputBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    old_binding = std::move(roblox_text_input_binding_);
  }
}

bool VM::DispatchRobloxTextInputShow(
    const RobloxTextInputShowRequest& request) {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.show == nullptr || request.text_box <= 0) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  // An identical show may reopen a TextBox closed without a hide callback.
  ClearSensitiveString(&binding->last_request.text);
  binding->last_request = request;
  binding->active = true;
  binding->callbacks.show(binding->context.get(), request);
  return true;
}

bool VM::DispatchRobloxTextInputHide() {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.hide == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  if (!binding->active) {
    return true;
  }
  binding->active = false;
  ClearSensitiveString(&binding->last_request.text);
  binding->last_request = {};
  binding->callbacks.hide(binding->context.get());
  return true;
}

bool VM::DispatchRobloxTextInputReplaceText(const std::string& text) {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.replace_text == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  if (!binding->active || binding->last_request.text == text) {
    return true;
  }
  ClearSensitiveString(&binding->last_request.text);
  binding->last_request.text = text;
  binding->callbacks.replace_text(binding->context.get(), text);
  return true;
}

bool VM::DispatchRobloxTextInputPropertiesChanged() {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.properties_changed == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  if (!binding->active) {
    return true;
  }
  binding->callbacks.properties_changed(binding->context.get());
  return true;
}

jobject
VM::CreateMessageBusRawCallback(std::shared_ptr<void> context,
                                const MessageBusRawCallbacks &callbacks) {
  if (context == nullptr || callbacks.run == nullptr) {
    return nullptr;
  }
  JNIEnv *env = GetJNIEnv();
  if (env == nullptr) {
    return nullptr;
  }
  jclass callback_class =
      env->FindClass("com/roblox/universalapp/messagebus/RawCallback");
  if (callback_class == nullptr) {
    return nullptr;
  }
  jobject callback = env->AllocObject(callback_class);
  env->DeleteLocalRef(callback_class);
  if (callback == nullptr) {
    return nullptr;
  }
  auto binding = std::make_shared<MessageBusRawBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    message_bus_raw_bindings_[callback] = std::move(binding);
  }
  return callback;
}

void VM::ClearMessageBusRawCallback(jobject callback) {
  std::shared_ptr<MessageBusRawBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    const auto found = message_bus_raw_bindings_.find(callback);
    if (found == message_bus_raw_bindings_.end()) {
      return;
    }
    old_binding = std::move(found->second);
    message_bus_raw_bindings_.erase(found);
  }
}

bool VM::DispatchMessageBusRawCallback(jobject callback, JNIEnv *env,
                                       jstring message) {
  std::shared_ptr<MessageBusRawBinding> binding;
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    const auto found = message_bus_raw_bindings_.find(callback);
    if (found != message_bus_raw_bindings_.end()) {
      binding = found->second;
    }
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.run == nullptr || env == nullptr ||
      message == nullptr) {
    return false;
  }
  binding->callbacks.run(binding->context.get(), env, message);
  return true;
}

jobject VM::CreateMessageBusRequestHandler(
    std::shared_ptr<void> context,
    const MessageBusRequestHandlerCallbacks& callbacks) {
  if (context == nullptr || callbacks.run == nullptr) {
    return nullptr;
  }
  JNIEnv* env = GetJNIEnv();
  jclass handler_class = env != nullptr
                             ? env->FindClass(
                                   "com/roblox/universalapp/messagebus/"
                                   "RequestHandlerRaw")
                             : nullptr;
  if (handler_class == nullptr) {
    return nullptr;
  }
  jobject handler = env->AllocObject(handler_class);
  env->DeleteLocalRef(handler_class);
  if (handler == nullptr) {
    return nullptr;
  }
  auto binding = std::make_shared<MessageBusRequestHandlerBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
  message_bus_request_handler_bindings_[handler] = std::move(binding);
  return handler;
}

void VM::ClearMessageBusRequestHandler(jobject handler) {
  std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
  message_bus_request_handler_bindings_.erase(handler);
}

jstring VM::DispatchMessageBusRequestHandler(jobject handler, JNIEnv* env,
                                             jstring message) {
  std::shared_ptr<MessageBusRequestHandlerBinding> binding;
  {
    std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
    const auto found = message_bus_request_handler_bindings_.find(handler);
    if (found != message_bus_request_handler_bindings_.end()) {
      binding = found->second;
    }
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.run == nullptr || env == nullptr) {
    return nullptr;
  }
  const std::string response =
      binding->callbacks.run(binding->context.get(), env, message);
  return env->NewStringUTF(response.c_str());
}

jobject VM::CreateMessageBusAsyncRequestHandler(
    std::shared_ptr<void> context,
    const MessageBusAsyncRequestHandlerCallbacks& callbacks) {
  if (context == nullptr || callbacks.run == nullptr) return nullptr;
  JNIEnv* env = GetJNIEnv();
  if (env == nullptr) return nullptr;
  jclass cls = env->FindClass(
      "com/roblox/universalapp/messagebus/RequestHandlerAsyncRaw");
  if (cls == nullptr) return nullptr;
  jobject handler = env->AllocObject(cls);
  env->DeleteLocalRef(cls);
  if (handler == nullptr) return nullptr;
  auto binding = std::make_shared<MessageBusAsyncRequestHandlerBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  std::lock_guard<std::mutex> lock(message_bus_async_request_handler_mutex_);
  message_bus_async_request_handler_bindings_[handler] = std::move(binding);
  return handler;
}

void VM::ClearMessageBusAsyncRequestHandler(jobject handler) {
  std::shared_ptr<MessageBusAsyncRequestHandlerBinding> removed;
  {
    std::lock_guard<std::mutex> lock(message_bus_async_request_handler_mutex_);
    const auto found =
        message_bus_async_request_handler_bindings_.find(handler);
    if (found == message_bus_async_request_handler_bindings_.end()) return;
    removed = std::move(found->second);
    message_bus_async_request_handler_bindings_.erase(found);
  }
}

bool VM::DispatchMessageBusAsyncRequestHandler(jobject handler, JNIEnv* env,
                                               jstring message,
                                               jstring response_id) {
  std::shared_ptr<MessageBusAsyncRequestHandlerBinding> binding;
  {
    std::lock_guard<std::mutex> lock(message_bus_async_request_handler_mutex_);
    const auto found =
        message_bus_async_request_handler_bindings_.find(handler);
    if (found != message_bus_async_request_handler_bindings_.end()) {
      binding = found->second;
    }
  }
  if (binding == nullptr || env == nullptr || response_id == nullptr) {
    return false;
  }
  binding->callbacks.run(binding->context.get(), env, message, response_id);
  return true;
}

jobject VM::CreateMemStorageCallback(
    std::shared_ptr<void> context,
    const MemStorageCallbackCallbacks& callbacks) {
  if (context == nullptr || callbacks.on_item_set == nullptr) {
    return nullptr;
  }
  JNIEnv* env = GetJNIEnv();
  if (env == nullptr) {
    return nullptr;
  }
  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/memstorage/Callback");
  if (callback_class == nullptr) {
    return nullptr;
  }
  jobject callback = env->AllocObject(callback_class);
  env->DeleteLocalRef(callback_class);
  if (callback == nullptr) {
    return nullptr;
  }

  auto binding = std::make_shared<MemStorageCallbackBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    mem_storage_callback_bindings_[callback] = std::move(binding);
  }
  return callback;
}

void VM::ClearMemStorageCallback(jobject callback) {
  std::shared_ptr<MemStorageCallbackBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    const auto found = mem_storage_callback_bindings_.find(callback);
    if (found == mem_storage_callback_bindings_.end()) {
      return;
    }
    old_binding = std::move(found->second);
    mem_storage_callback_bindings_.erase(found);
  }
}

bool VM::DispatchMemStorageCallback(jobject callback, JNIEnv *env,
                                    jstring value) {
  std::shared_ptr<MemStorageCallbackBinding> binding;
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    const auto found = mem_storage_callback_bindings_.find(callback);
    if (found != mem_storage_callback_bindings_.end()) {
      binding = found->second;
    }
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_item_set == nullptr || env == nullptr ||
      value == nullptr) {
    return false;
  }
  binding->callbacks.on_item_set(binding->context.get(), env, value);
  return true;
}

void VM::SetRobloxDataModelNotificationCallbacks(
    std::shared_ptr<void> context,
    const RobloxDataModelNotificationCallbacks &callbacks) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  if (context != nullptr && callbacks.on_notification != nullptr) {
    binding = std::make_shared<RobloxDataModelNotificationBinding>();
    binding->context = std::move(context);
    binding->callbacks = callbacks;
  }
  std::shared_ptr<RobloxDataModelNotificationBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    old_binding = std::move(roblox_data_model_notification_binding_);
    roblox_data_model_notification_binding_ = std::move(binding);
  }
}

void VM::ClearRobloxDataModelNotificationCallbacks() {
  std::shared_ptr<RobloxDataModelNotificationBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    old_binding = std::move(roblox_data_model_notification_binding_);
  }
}

bool VM::DispatchRobloxDataModelNotification(JNIEnv *env, jstring type,
                                             jstring data) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_notification == nullptr || env == nullptr ||
      type == nullptr || data == nullptr) {
    return false;
  }
  binding->callbacks.on_notification(binding->context.get(), env, type, data);
  return true;
}

bool VM::DispatchRobloxAppBridgeNotification(JNIEnv* env, jstring type,
                                             jstring data) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_app_bridge_notification == nullptr ||
      env == nullptr || type == nullptr || data == nullptr) {
    return false;
  }
  binding->callbacks.on_app_bridge_notification(binding->context.get(), env,
                                                type, data);
  return true;
}

bool VM::DispatchRobloxNativeOverlay(JNIEnv* env, jstring title, jstring url) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_native_overlay == nullptr || env == nullptr ||
      title == nullptr || url == nullptr) {
    return false;
  }
  binding->callbacks.on_native_overlay(binding->context.get(), env, title, url);
  return true;
}

bool VM::DispatchRobloxOpenWebActivity(JNIEnv* env, jstring url,
                                       jstring title) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_open_web_activity == nullptr || env == nullptr ||
      url == nullptr || title == nullptr) {
    return false;
  }
  binding->callbacks.on_open_web_activity(binding->context.get(), env, url,
                                          title);
  return true;
}

bool VM::DispatchRobloxCookieSync(JNIEnv* env, jstring cookie) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_sync_cookies == nullptr || env == nullptr ||
      cookie == nullptr) {
    return false;
  }
  binding->callbacks.on_sync_cookies(binding->context.get(), env, cookie);
  return true;
}

bool VM::DispatchRobloxCookieSet(JNIEnv* env, jobjectArray cookies,
                                jstring url) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_set_cookie == nullptr || env == nullptr ||
      cookies == nullptr || url == nullptr) {
    return false;
  }
  const jsize cookie_count = env->GetArrayLength(cookies);
  if (cookie_count < 0 || cookie_count > kMaximumCookieSetCount) {
    return false;
  }
  for (jsize index = 0; index < cookie_count; ++index) {
    auto cookie =
        static_cast<jstring>(env->GetObjectArrayElement(cookies, index));
    if (cookie == nullptr) {
      continue;
    }
    const jsize cookie_size = env->GetStringUTFLength(cookie);
    if (cookie_size <= 0 || cookie_size > kMaximumCookieSetBytes) {
      env->DeleteLocalRef(cookie);
      continue;
    }
    const char* cookie_chars = env->GetStringUTFChars(cookie, nullptr);
    if (cookie_chars == nullptr) {
      env->DeleteLocalRef(cookie);
      continue;
    }
    std::string raw_cookie(cookie_chars,
                           static_cast<std::size_t>(cookie_size));
    env->ReleaseStringUTFChars(cookie, cookie_chars);
    env->DeleteLocalRef(cookie);
    std::string canonical_cookie = NormalizeCookieHeader(raw_cookie);
    ClearCookieString(&raw_cookie);
    if (canonical_cookie.empty()) {
      continue;
    }
    StoreCookieHeader(canonical_cookie);
    jstring callback_cookie = env->NewStringUTF(canonical_cookie.c_str());
    ClearCookieString(&canonical_cookie);
    if (callback_cookie == nullptr) {
      continue;
    }
    binding->callbacks.on_set_cookie(binding->context.get(), env,
                                     callback_cookie, url);
    env->DeleteLocalRef(callback_cookie);
  }
  return true;
}

void VM::SetRobloxExperienceLifecycleCallbacks(
    std::shared_ptr<void> context,
    const RobloxExperienceLifecycleCallbacks &callbacks) {
  std::shared_ptr<RobloxExperienceLifecycleBinding> binding;
  if (context != nullptr && callbacks.on_lua_app_did_return != nullptr) {
    binding = std::make_shared<RobloxExperienceLifecycleBinding>();
    binding->context = std::move(context);
    binding->callbacks = callbacks;
  }
  std::shared_ptr<RobloxExperienceLifecycleBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_experience_lifecycle_mutex_);
    old_binding = std::move(roblox_experience_lifecycle_binding_);
    roblox_experience_lifecycle_binding_ = std::move(binding);
  }
}

void VM::ClearRobloxExperienceLifecycleCallbacks() {
  std::shared_ptr<RobloxExperienceLifecycleBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_experience_lifecycle_mutex_);
    old_binding = std::move(roblox_experience_lifecycle_binding_);
  }
}

bool VM::DispatchRobloxExperienceLuaAppDidReturn() {
  std::shared_ptr<RobloxExperienceLifecycleBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_experience_lifecycle_mutex_);
    binding = roblox_experience_lifecycle_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_lua_app_did_return == nullptr) {
    return false;
  }
  binding->callbacks.on_lua_app_did_return(binding->context.get());
  return true;
}

void VM::SetFmodAudioDeviceCallbacks(
    std::shared_ptr<void> context,
    const FmodAudioDeviceCallbacks& callbacks) {
  FmodAudioDeviceBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    old_binding = std::move(fmod_audio_device_binding_);
    fmod_audio_device_binding_.context = std::move(context);
    fmod_audio_device_binding_.callbacks = callbacks;
  }
  if (old_binding.context != nullptr &&
      old_binding.callbacks.shutdown != nullptr) {
    old_binding.callbacks.shutdown(old_binding.context.get());
  }
}

void VM::ClearFmodAudioDeviceCallbacks() {
  FmodAudioDeviceBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    old_binding = std::move(fmod_audio_device_binding_);
    fmod_audio_device_binding_ = {};
  }
  if (old_binding.context != nullptr &&
      old_binding.callbacks.shutdown != nullptr) {
    old_binding.callbacks.shutdown(old_binding.context.get());
  }
}

bool VM::DispatchFmodAudioDeviceInit(const void* identity, int channels,
                                     int sample_rate_hz,
                                     int block_size_frames,
                                     int block_count) {
  FmodAudioDeviceBinding binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    binding = fmod_audio_device_binding_;
  }
  return binding.context != nullptr && binding.callbacks.init != nullptr &&
         binding.callbacks.init(binding.context.get(), identity, channels,
                                sample_rate_hz, block_size_frames,
                                block_count);
}

bool VM::DispatchFmodAudioDeviceWrite(const void* identity,
                                      const std::uint8_t* data,
                                      std::size_t size) {
  FmodAudioDeviceBinding binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    binding = fmod_audio_device_binding_;
  }
  return binding.context != nullptr && binding.callbacks.write != nullptr &&
         binding.callbacks.write(binding.context.get(), identity, data, size);
}

bool VM::DispatchFmodAudioDeviceClose(const void* identity) {
  FmodAudioDeviceBinding binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    binding = fmod_audio_device_binding_;
  }
  return binding.context != nullptr && binding.callbacks.close != nullptr &&
         binding.callbacks.close(binding.context.get(), identity);
}

namespace {

void OnWebRtcAudioRecordData(void *context, const void *identity,
                             std::size_t size_bytes) {
  auto *vm = static_cast<VM *>(context);
  if (vm != nullptr) {
    vm->DispatchWebRtcAudioRecordData(identity, size_bytes);
  }
}

} // namespace

void VM::SetWebRtcAudioManagerCallbacks(
    std::shared_ptr<void> context,
    const WebRtcAudioManagerCallbacks& callbacks) {
  WebRtcAudioManagerBinding previous;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_manager_mutex_);
    previous = std::move(webrtc_audio_manager_binding_);
    webrtc_audio_manager_binding_ = {std::move(context), callbacks};
  }
}

void VM::ClearWebRtcAudioManagerCallbacks() {
  WebRtcAudioManagerBinding previous;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_manager_mutex_);
    previous = std::move(webrtc_audio_manager_binding_);
    webrtc_audio_manager_binding_ = {};
  }
}

void VM::RegisterWebRtcAudioManagerNative(const char* name,
                                          const char* signature,
                                          void* function) {
  if (name == nullptr || signature == nullptr || function == nullptr ||
      std::strcmp(name, "nativeCacheAudioParameters") != 0 ||
      std::strcmp(signature, "(IIIZZZZZZZIIJ)V") != 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(webrtc_audio_manager_mutex_);
  webrtc_cache_audio_parameters_native_ = function;
}

bool VM::DispatchWebRtcAudioManagerConstruct(jobject manager,
                                             jlong native_audio_manager) {
  const auto fail = [](const char* reason) {
    std::cerr << "  [mocktail][audio] WebRTC audio manager parameters_failed "
              << "reason=" << reason << '\n';
    return false;
  };
  if (ObjectClassName(manager) != "org/webrtc/voiceengine/WebRtcAudioManager") {
    return fail("invalid receiver");
  }
  SetLongFieldRaw(manager, "nativeAudioManager", native_audio_manager);
  SetBooleanFieldRaw(manager, "initialized", JNI_FALSE);
  SetBooleanFieldRaw(manager, "mocktailAudioParametersCached", JNI_FALSE);
  WebRtcAudioManagerBinding binding;
  void* native = nullptr;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_manager_mutex_);
    binding = webrtc_audio_manager_binding_;
    native = webrtc_cache_audio_parameters_native_;
  }
  if (native == nullptr || native_audio_manager == 0) {
    return fail("missing nativeCacheAudioParameters or native handle");
  }
  WebRtcAudioManagerParameters parameters;
  if (binding.context == nullptr ||
      binding.callbacks.get_parameters == nullptr ||
      !binding.callbacks.get_parameters(binding.context.get(), &parameters)) {
    return fail("host audio manager unavailable");
  }
  if (parameters.sample_rate_hz <= 0 || parameters.output_channels < 1 ||
      parameters.output_channels > 2 || parameters.input_channels < 1 ||
      parameters.input_channels > 2 ||
      parameters.output_buffer_size_frames <= 0 ||
      parameters.input_buffer_size_frames <= 0) {
    return fail("invalid host PCM parameters");
  }
  SetIntFieldRaw(manager, "sampleRate", parameters.sample_rate_hz);
  SetIntFieldRaw(manager, "outputChannels", parameters.output_channels);
  SetIntFieldRaw(manager, "inputChannels", parameters.input_channels);
  SetIntFieldRaw(manager, "outputBufferSize",
                 parameters.output_buffer_size_frames);
  SetIntFieldRaw(manager, "inputBufferSize",
                 parameters.input_buffer_size_frames);
  SetBooleanFieldRaw(manager, "lowLatencyOutput",
                     parameters.low_latency_output);
  SetBooleanFieldRaw(manager, "lowLatencyInput", parameters.low_latency_input);
  SetBooleanFieldRaw(manager, "proAudio", parameters.pro_audio);
  SetBooleanFieldRaw(manager, "aAudio", parameters.aaudio);

  // Execute the constructor's Java -> native callback before returning the
  // object. The guest needs these formats even before it calls init(). No VM
  // locks are held here: the native callback may make nested JNI calls.
  using CacheAudioParameters = void(JNICALL*)(
      JNIEnv*, jobject, jint, jint, jint, jboolean, jboolean, jboolean,
      jboolean, jboolean, jboolean, jboolean, jint, jint, jlong);
  reinterpret_cast<CacheAudioParameters>(native)(
      GetJNIEnv(), manager, parameters.sample_rate_hz,
      parameters.output_channels, parameters.input_channels,
      parameters.hardware_aec, parameters.hardware_agc, parameters.hardware_ns,
      parameters.low_latency_output, parameters.low_latency_input,
      parameters.pro_audio, parameters.aaudio,
      parameters.output_buffer_size_frames, parameters.input_buffer_size_frames,
      native_audio_manager);
  SetBooleanFieldRaw(manager, "mocktailAudioParametersCached", JNI_TRUE);
  std::cout << "  [mocktail][audio] WebRTC audio manager parameters "
            << "sample_rate_hz=" << parameters.sample_rate_hz
            << " output_channels=" << parameters.output_channels
            << " input_channels=" << parameters.input_channels << '\n'
            << std::flush;
  return true;
}

bool VM::DispatchWebRtcAudioManagerInit(jobject manager) {
  WebRtcAudioManagerBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_manager_mutex_);
    binding = webrtc_audio_manager_binding_;
  }
  const bool initialized =
      ObjectClassName(manager) == "org/webrtc/voiceengine/WebRtcAudioManager" &&
      BooleanFieldValue(manager, "mocktailAudioParametersCached") == JNI_TRUE &&
      binding.context != nullptr && binding.callbacks.init != nullptr &&
      (BooleanFieldValue(manager, "initialized") == JNI_TRUE ||
       binding.callbacks.init(binding.context.get(), manager));
  SetBooleanFieldRaw(manager, "initialized",
                     initialized ? JNI_TRUE : JNI_FALSE);
  if (!initialized) {
    std::cerr << "  [mocktail][audio] WebRTC audio manager init_failed "
                 "reason=parameters or host manager unavailable\n";
  }
  return initialized;
}

void VM::DispatchWebRtcAudioManagerDispose(jobject manager) {
  if (ObjectClassName(manager) != "org/webrtc/voiceengine/WebRtcAudioManager" ||
      BooleanFieldValue(manager, "initialized") != JNI_TRUE) {
    return;
  }
  SetBooleanFieldRaw(manager, "initialized", JNI_FALSE);
  WebRtcAudioManagerBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_manager_mutex_);
    binding = webrtc_audio_manager_binding_;
  }
  if (binding.context != nullptr && binding.callbacks.dispose != nullptr) {
    binding.callbacks.dispose(binding.context.get(), manager);
  }
}

void VM::DispatchWebRtcAudioManagerMicrophoneMute(jobject manager, bool muted) {
  if (ObjectClassName(manager) != "org/webrtc/voiceengine/WebRtcAudioManager" ||
      BooleanFieldValue(manager, "mocktailAudioParametersCached") != JNI_TRUE) {
    return;
  }
  WebRtcAudioManagerBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_manager_mutex_);
    binding = webrtc_audio_manager_binding_;
  }
  if (binding.context != nullptr &&
      binding.callbacks.set_microphone_mute != nullptr) {
    binding.callbacks.set_microphone_mute(binding.context.get(), muted);
  }
}

void VM::SetWebRtcAudioRecordCallbacks(
    std::shared_ptr<void> context,
    const WebRtcAudioRecordCallbacks &callbacks) {
  WebRtcAudioRecordBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_mutex_);
    old_binding = std::move(webrtc_audio_record_binding_);
    webrtc_audio_record_binding_.context = std::move(context);
    webrtc_audio_record_binding_.callbacks = callbacks;
  }
  if (old_binding.context != nullptr &&
      old_binding.callbacks.shutdown != nullptr) {
    old_binding.callbacks.shutdown(old_binding.context.get());
  }
}

void VM::ClearWebRtcAudioRecordCallbacks() {
  WebRtcAudioRecordBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_mutex_);
    old_binding = std::move(webrtc_audio_record_binding_);
    webrtc_audio_record_binding_ = {};
  }
  if (old_binding.context != nullptr &&
      old_binding.callbacks.shutdown != nullptr) {
    old_binding.callbacks.shutdown(old_binding.context.get());
  }
}

int VM::DispatchWebRtcAudioRecordInit(const void *identity, int sample_rate_hz,
                                      int channels, void **direct_buffer,
                                      std::size_t *direct_buffer_capacity) {
  WebRtcAudioRecordBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_mutex_);
    binding = webrtc_audio_record_binding_;
  }
  if (binding.context == nullptr || binding.callbacks.init == nullptr) {
    return -1;
  }
  const int frames = binding.callbacks.init(
      binding.context.get(), identity, sample_rate_hz, channels,
      &OnWebRtcAudioRecordData, this, direct_buffer, direct_buffer_capacity);
  if (frames < 0 || direct_buffer == nullptr || *direct_buffer == nullptr ||
      direct_buffer_capacity == nullptr || *direct_buffer_capacity == 0) {
    return -1;
  }

  void *native = nullptr;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_native_mutex_);
    native = webrtc_cache_direct_buffer_native_;
  }
  const jlong native_audio_record =
      LongFieldValue(reinterpret_cast<jobject>(const_cast<void *>(identity)),
                     "nativeAudioRecord");
  if (native == nullptr || native_audio_record == 0 ||
      *direct_buffer_capacity >
          static_cast<std::size_t>(std::numeric_limits<jlong>::max())) {
    if (binding.callbacks.close != nullptr) {
      binding.callbacks.close(binding.context.get(), identity);
    }
    return -1;
  }
  JNIEnv *env = GetJNIEnv();
  jobject byte_buffer = env->NewDirectByteBuffer(
      *direct_buffer, static_cast<jlong>(*direct_buffer_capacity));
  if (byte_buffer == nullptr) {
    if (binding.callbacks.close != nullptr) {
      binding.callbacks.close(binding.context.get(), identity);
    }
    return -1;
  }
  using CacheDirectBuffer = void(JNICALL *)(JNIEnv *, jobject, jobject, jlong);
  reinterpret_cast<CacheDirectBuffer>(native)(
      env, reinterpret_cast<jobject>(const_cast<void *>(identity)), byte_buffer,
      native_audio_record);
  return frames;
}

bool VM::DispatchWebRtcAudioRecordStart(const void *identity) {
  WebRtcAudioRecordBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_mutex_);
    binding = webrtc_audio_record_binding_;
  }
  return binding.context != nullptr && binding.callbacks.start != nullptr &&
         binding.callbacks.start(binding.context.get(), identity);
}

bool VM::DispatchWebRtcAudioRecordStop(const void *identity) {
  WebRtcAudioRecordBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_mutex_);
    binding = webrtc_audio_record_binding_;
  }
  return binding.context != nullptr && binding.callbacks.stop != nullptr &&
         binding.callbacks.stop(binding.context.get(), identity);
}

void VM::DispatchWebRtcAudioRecordClose(const void *identity) {
  WebRtcAudioRecordBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_mutex_);
    binding = webrtc_audio_record_binding_;
  }
  if (binding.context != nullptr && binding.callbacks.close != nullptr) {
    binding.callbacks.close(binding.context.get(), identity);
  }
}

void VM::RegisterWebRtcAudioRecordNative(const char *name,
                                         const char *signature,
                                         void *function) {
  if (name == nullptr || signature == nullptr || function == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(webrtc_audio_record_native_mutex_);
  if (std::strcmp(name, "nativeCacheDirectBufferAddress") == 0 &&
      std::strcmp(signature, "(Ljava/nio/ByteBuffer;J)V") == 0) {
    webrtc_cache_direct_buffer_native_ = function;
  } else if (std::strcmp(name, "nativeDataIsRecorded") == 0 &&
             std::strcmp(signature, "(IJ)V") == 0) {
    webrtc_data_is_recorded_native_ = function;
  }
}

void VM::DispatchWebRtcAudioRecordData(const void *identity,
                                       std::size_t size_bytes) {
  void *native = nullptr;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_record_native_mutex_);
    native = webrtc_data_is_recorded_native_;
  }
  if (native == nullptr || identity == nullptr ||
      size_bytes > static_cast<std::size_t>(std::numeric_limits<jint>::max())) {
    return;
  }
  const jlong native_audio_record =
      LongFieldValue(reinterpret_cast<jobject>(const_cast<void *>(identity)),
                     "nativeAudioRecord");
  if (native_audio_record == 0) {
    return;
  }
  using DataIsRecorded = void(JNICALL *)(JNIEnv *, jobject, jint, jlong);
  reinterpret_cast<DataIsRecorded>(native)(
      GetJNIEnv(), reinterpret_cast<jobject>(const_cast<void *>(identity)),
      static_cast<jint>(size_bytes), native_audio_record);
}

namespace {

void OnWebRtcAudioTrackData(void *context, const void *identity,
                            std::size_t size_bytes) {
  auto *vm = static_cast<VM *>(context);
  if (vm != nullptr) {
    vm->DispatchWebRtcAudioTrackData(identity, size_bytes);
  }
}

} // namespace

void VM::SetWebRtcAudioTrackCallbacks(
    std::shared_ptr<void> context, const WebRtcAudioTrackCallbacks &callbacks) {
  std::lock_guard<std::mutex> lock(webrtc_audio_track_mutex_);
  webrtc_audio_track_binding_.context = std::move(context);
  webrtc_audio_track_binding_.callbacks = callbacks;
}

void VM::ClearWebRtcAudioTrackCallbacks() {
  std::lock_guard<std::mutex> lock(webrtc_audio_track_mutex_);
  webrtc_audio_track_binding_ = {};
}

int VM::DispatchWebRtcAudioTrackInit(const void *identity, int sample_rate_hz,
                                     int channels, double buffer_size_factor,
                                     void **direct_buffer,
                                     std::size_t *direct_buffer_capacity) {
  WebRtcAudioTrackBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_track_mutex_);
    binding = webrtc_audio_track_binding_;
  }
  if (binding.context == nullptr || binding.callbacks.init == nullptr ||
      direct_buffer == nullptr || direct_buffer_capacity == nullptr) {
    return -1;
  }
  const int buffer_size_bytes = binding.callbacks.init(
      binding.context.get(), identity, sample_rate_hz, channels,
      buffer_size_factor, &OnWebRtcAudioTrackData, this, direct_buffer,
      direct_buffer_capacity);
  if (buffer_size_bytes < 0 || *direct_buffer == nullptr ||
      *direct_buffer_capacity == 0 ||
      *direct_buffer_capacity >
          static_cast<std::size_t>(std::numeric_limits<jlong>::max())) {
    if (binding.callbacks.close != nullptr) {
      binding.callbacks.close(binding.context.get(), identity);
    }
    return -1;
  }

  void *native = nullptr;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_track_native_mutex_);
    native = webrtc_track_cache_direct_buffer_native_;
  }
  const jlong native_audio_track =
      LongFieldValue(reinterpret_cast<jobject>(const_cast<void *>(identity)),
                     "nativeAudioTrack");
  if (native == nullptr || native_audio_track == 0) {
    if (binding.callbacks.close != nullptr) {
      binding.callbacks.close(binding.context.get(), identity);
    }
    return -1;
  }

  JNIEnv *env = GetJNIEnv();
  jobject byte_buffer = env->NewDirectByteBuffer(
      *direct_buffer, static_cast<jlong>(*direct_buffer_capacity));
  if (byte_buffer == nullptr) {
    if (binding.callbacks.close != nullptr) {
      binding.callbacks.close(binding.context.get(), identity);
    }
    return -1;
  }
  using CacheDirectBuffer = void(JNICALL *)(JNIEnv *, jobject, jobject, jlong);
  reinterpret_cast<CacheDirectBuffer>(native)(
      env, reinterpret_cast<jobject>(const_cast<void *>(identity)), byte_buffer,
      native_audio_track);
  return buffer_size_bytes;
}

int VM::DispatchWebRtcAudioTrackBufferSizeFrames(const void *identity) {
  WebRtcAudioTrackBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_track_mutex_);
    binding = webrtc_audio_track_binding_;
  }
  return binding.context != nullptr &&
                 binding.callbacks.buffer_size_frames != nullptr
             ? binding.callbacks.buffer_size_frames(binding.context.get(),
                                                    identity)
             : 0;
}

bool VM::DispatchWebRtcAudioTrackStart(const void *identity) {
  WebRtcAudioTrackBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_track_mutex_);
    binding = webrtc_audio_track_binding_;
  }
  return binding.context != nullptr && binding.callbacks.start != nullptr &&
         binding.callbacks.start(binding.context.get(), identity);
}

bool VM::DispatchWebRtcAudioTrackStop(const void *identity) {
  WebRtcAudioTrackBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_track_mutex_);
    binding = webrtc_audio_track_binding_;
  }
  return binding.context != nullptr && binding.callbacks.stop != nullptr &&
         binding.callbacks.stop(binding.context.get(), identity);
}

void VM::DispatchWebRtcAudioTrackClose(const void *identity) {
  WebRtcAudioTrackBinding binding;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_track_mutex_);
    binding = webrtc_audio_track_binding_;
  }
  if (binding.context != nullptr && binding.callbacks.close != nullptr) {
    binding.callbacks.close(binding.context.get(), identity);
  }
}

void VM::RegisterWebRtcAudioTrackNative(const char *name, const char *signature,
                                        void *function) {
  if (name == nullptr || signature == nullptr || function == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(webrtc_audio_track_native_mutex_);
  if (std::strcmp(name, "nativeCacheDirectBufferAddress") == 0 &&
      std::strcmp(signature, "(Ljava/nio/ByteBuffer;J)V") == 0) {
    webrtc_track_cache_direct_buffer_native_ = function;
  } else if (std::strcmp(name, "nativeGetPlayoutData") == 0 &&
             std::strcmp(signature, "(IJ)V") == 0) {
    webrtc_get_playout_data_native_ = function;
  }
}

void VM::DispatchWebRtcAudioTrackData(const void *identity,
                                      std::size_t size_bytes) {
  void *native = nullptr;
  {
    std::lock_guard<std::mutex> lock(webrtc_audio_track_native_mutex_);
    native = webrtc_get_playout_data_native_;
  }
  if (native == nullptr || identity == nullptr ||
      size_bytes > static_cast<std::size_t>(std::numeric_limits<jint>::max())) {
    return;
  }
  const jlong native_audio_track =
      LongFieldValue(reinterpret_cast<jobject>(const_cast<void *>(identity)),
                     "nativeAudioTrack");
  if (native_audio_track == 0) {
    return;
  }
  using GetPlayoutData = void(JNICALL *)(JNIEnv *, jobject, jint, jlong);
  reinterpret_cast<GetPlayoutData>(native)(
      GetJNIEnv(), reinterpret_cast<jobject>(const_cast<void *>(identity)),
      static_cast<jint>(size_bytes), native_audio_track);
}

void VM::SetAndroidWindowCallbacks(
    std::shared_ptr<void> context,
    const AndroidWindowCallbacks& callbacks) {
  std::lock_guard<std::mutex> lock(android_window_mutex_);
  android_window_binding_.context = std::move(context);
  android_window_binding_.callbacks = callbacks;
}

void VM::ClearAndroidWindowCallbacks() {
  std::lock_guard<std::mutex> lock(android_window_mutex_);
  android_window_binding_ = {};
}

bool VM::DispatchAndroidWindowFlags(int flags, int mask) {
  AndroidWindowBinding binding;
  {
    std::lock_guard<std::mutex> lock(android_window_mutex_);
    binding = android_window_binding_;
  }
  return binding.context != nullptr && binding.callbacks.set_flags != nullptr &&
         binding.callbacks.set_flags(binding.context.get(), flags, mask);
}

void VM::SetRobloxAuthIdentity(const RobloxAuthIdentity& identity) {
  std::lock_guard<std::mutex> lock(roblox_auth_identity_mutex_);
  roblox_auth_identity_ =
      identity.user_id > 0 ? identity : RobloxAuthIdentity{};
}

void VM::ClearRobloxAuthIdentity() {
  std::lock_guard<std::mutex> lock(roblox_auth_identity_mutex_);
  roblox_auth_identity_ = {};
}

RobloxAuthIdentity VM::GetRobloxAuthIdentitySnapshot() const {
  std::lock_guard<std::mutex> lock(roblox_auth_identity_mutex_);
  return roblox_auth_identity_;
}

void VM::SetPlatformIdentity(const PlatformIdentity& identity) {
  std::lock_guard<std::mutex> lock(platform_identity_mutex_);
  platform_identity_ = identity;
}

PlatformIdentity VM::GetPlatformIdentitySnapshot() const {
  std::lock_guard<std::mutex> lock(platform_identity_mutex_);
  return platform_identity_;
}

void VM::SetRobloxCredentialProvider(const void* context,
                                     RobloxCredentialProvider provider) {
  {
    std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
    ClearCookieString(&roblox_credential_override_);
    roblox_credential_provider_context_ = context;
    roblox_credential_provider_ = provider;
  }
  if (provider != nullptr) {
    ClearLegacyCookieStore();
  }
}

void VM::ClearRobloxCredentialProvider() {
  std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
  ClearCookieString(&roblox_credential_override_);
  roblox_credential_provider_ = nullptr;
  roblox_credential_provider_context_ = nullptr;
}

bool VM::CopyRobloxCredentialFromProvider(std::string* credential) const {
  std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
  if (!roblox_credential_override_.empty()) {
    if (credential != nullptr) {
      credential->assign(roblox_credential_override_);
    }
    return true;
  }
  if (roblox_credential_provider_ == nullptr) {
    if (credential != nullptr) {
      credential->clear();
    }
    return false;
  }
  if (credential == nullptr) {
    return true;
  }
  const RobloxCredentialView view =
      roblox_credential_provider_(roblox_credential_provider_context_);
  if (view.data == nullptr || view.size == 0) {
    credential->clear();
  } else {
    credential->assign(view.data, view.size);
  }
  return true;
}

void VM::SetRobloxCookieGetter(RobloxCookieGetter getter) {
  std::lock_guard<std::mutex> lock(roblox_cookie_getter_mutex_);
  roblox_cookie_getter_ = getter;
}

void VM::SetRobloxAccountSwitchHandler(void* context,
                                       RobloxAccountSwitchHandler handler) {
  std::lock_guard<std::mutex> lock(roblox_cookie_getter_mutex_);
  roblox_account_switch_context_ = context;
  roblox_account_switch_handler_ = handler;
}

void VM::DispatchRobloxAccountSwitch(bool signed_in) {
  void* context = nullptr;
  RobloxAccountSwitchHandler handler = nullptr;
  {
    std::lock_guard<std::mutex> lock(roblox_cookie_getter_mutex_);
    context = roblox_account_switch_context_;
    handler = roblox_account_switch_handler_;
  }
  if (handler != nullptr) {
    handler(context, signed_in);
  }
}

void VM::SetRobloxStorePurchaseHandler(void* context,
                                       RobloxStorePurchaseHandler handler) {
  std::lock_guard<std::mutex> lock(roblox_store_purchase_mutex_);
  roblox_store_purchase_context_ = context;
  roblox_store_purchase_handler_ = handler;
}

void VM::DispatchRobloxStorePurchase(jlong player_id,
                                     const std::string& product_id) {
  void* context = nullptr;
  RobloxStorePurchaseHandler handler = nullptr;
  {
    std::lock_guard<std::mutex> lock(roblox_store_purchase_mutex_);
    context = roblox_store_purchase_context_;
    handler = roblox_store_purchase_handler_;
  }
  if (handler != nullptr) {
    handler(context, player_id, product_id);
  }
}

bool VM::RefreshRobloxCredentialFromEngine(JNIEnv* env) {
  RobloxCookieGetter getter = nullptr;
  {
    std::lock_guard<std::mutex> lock(roblox_cookie_getter_mutex_);
    getter = roblox_cookie_getter_;
  }
  if (getter == nullptr || env == nullptr) {
    return false;
  }

  jstring domain = env->NewStringUTF("https://www.roblox.com/");
  if (domain == nullptr) {
    return false;
  }
  jstring cookies = getter(env, nullptr, domain);
  env->DeleteLocalRef(domain);
  if (cookies == nullptr) {
    return false;
  }

  std::string raw_cookie = StringFromJString(cookies);
  env->DeleteLocalRef(cookies);
  std::string canonical_cookie = NormalizeCookieHeader(raw_cookie);
  ClearCookieString(&raw_cookie);
  if (canonical_cookie.empty()) {
    return false;
  }
  StoreCookieHeader(canonical_cookie);
  ClearCookieString(&canonical_cookie);
  return true;
}

void VM::SetRobloxCredentialSink(
    std::shared_ptr<void> context,
    const RobloxCredentialSinkCallbacks& callbacks) {
  std::lock_guard<std::mutex> lock(roblox_credential_sink_mutex_);
  roblox_credential_sink_binding_.context = std::move(context);
  roblox_credential_sink_binding_.callbacks = callbacks;
}

void VM::ClearRobloxCredentialSink() {
  RobloxCredentialSinkBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_credential_sink_mutex_);
    old_binding = std::move(roblox_credential_sink_binding_);
    roblox_credential_sink_binding_ = {};
  }
}

bool VM::DispatchRobloxCredential(const char* data, std::size_t size) {
  RobloxCredentialSinkBinding binding;
  {
    std::lock_guard<std::mutex> lock(roblox_credential_sink_mutex_);
    binding = roblox_credential_sink_binding_;
  }
  const bool stored =
      binding.context != nullptr && binding.callbacks.store != nullptr &&
      data != nullptr && size != 0 &&
      binding.callbacks.store(binding.context.get(), data, size);
  if (!stored) {
    return false;
  }

  std::string accepted(data, size);
  {
    std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
    ClearCookieString(&roblox_credential_override_);
    roblox_credential_override_ = std::move(accepted);
  }
  ClearCookieString(&accepted);
  return true;
}

JNIEnv* VM::GetJNIEnv() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (JniVmTraceEnabled()) {
    std::cout << "  [JNI] GetJNIEnv enter\n";
  }
  if (!jni_env_) {
    jni_env_ = &jni_env_storage_;
  }
  jni_env_->functions = &native_interface_;
  if (g_thread_vm_instance == this && IsThreadLocalEnvValid()) {
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetJNIEnv thread-local hit\n";
    }
    return g_thread_local_env;
  }
  g_thread_audio_fp_mode.Restore();
  g_thread_vm_instance = this;
  g_thread_env_storage.functions = &native_interface_;
  g_thread_local_env = &g_thread_env_storage;
  if (JniVmTraceEnabled()) {
    std::cout << "  [JNI] GetJNIEnv attached thread-local env\n";
  }
  return g_thread_local_env;
}

void VM::RestoreFunctions() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  // JNI_OnLoad replaces env->functions; restore all known environments.
  java_vm_storage_.functions = &invoke_interface_;
  java_vm_ = &java_vm_storage_;
  if (jni_env_) {
    jni_env_->functions = &native_interface_;
  }
  if (g_thread_vm_instance == this && IsThreadLocalEnvValid()) {
    g_thread_local_env->functions = &native_interface_;
  }
}

std::shared_ptr<Class> VM::RegisterClass(const std::string& class_name) {
  if (JniVmTraceEnabled()) {
    fprintf(stderr, "  [JNI-VM] RegisterClass this=%p name_ref=%p name=\"%s\"\n",
            static_cast<void*>(this), static_cast<const void*>(&class_name),
            class_name.c_str());
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = class_registry_.find(class_name);
  if (it != class_registry_.end()) {
    return it->second;
  }
  auto cls = std::make_shared<Class>(class_name);
  class_registry_[class_name] = cls;
  return cls;
}

std::shared_ptr<Class> VM::FindClass(const std::string& class_name) const {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = class_registry_.find(class_name);
  if (it == class_registry_.end()) {
    return nullptr;
  }
  return it->second;
}

namespace {

using AnyFunction = void (*)();
using ZeroWord = std::uintptr_t;
// Float and double results come back in a vector register, not rax.
using ZeroReal = double;

void ReportUnimplementedJniFunction(const char* name,
                                    std::atomic<bool>& reported) {
  if (!reported.exchange(true)) {
    std::fprintf(stderr, "  [JNI] %s is not implemented; returning zero\n",
                 name);
  }
}

// Fills a slot with a function that ignores its arguments and returns zero.
// The caller owns its argument registers and stack, so ignoring them is safe.
// A void result ignores the value left in rax.
#define MOCKTAIL_JNI_STUB(name, result)                                      \
  table.name = reinterpret_cast<decltype(table.name)>(                       \
      reinterpret_cast<AnyFunction>(+[]() -> result {                        \
        static std::atomic<bool> reported{false};                            \
        ReportUnimplementedJniFunction(#name, reported);                     \
        return result{};                                                     \
      }))

#define MOCKTAIL_JNI_STUB_NONVIRTUAL(type, result)                           \
  MOCKTAIL_JNI_STUB(CallNonvirtual##type##Method, result);                   \
  MOCKTAIL_JNI_STUB(CallNonvirtual##type##MethodV, result);                  \
  MOCKTAIL_JNI_STUB(CallNonvirtual##type##MethodA, result)

#define MOCKTAIL_JNI_STUB_ARRAY(type)                                        \
  MOCKTAIL_JNI_STUB(New##type##Array, ZeroWord);                             \
  MOCKTAIL_JNI_STUB(Get##type##ArrayElements, ZeroWord);                     \
  MOCKTAIL_JNI_STUB(Release##type##ArrayElements, ZeroWord);                 \
  MOCKTAIL_JNI_STUB(Get##type##ArrayRegion, ZeroWord);                       \
  MOCKTAIL_JNI_STUB(Set##type##ArrayRegion, ZeroWord)

// Installed before the real functions, which replace their stubs. A slot left
// null would send the guest to address 0 when it calls the function.
void InstallUnimplementedJniFunctions(JNINativeInterface_& table) {
  MOCKTAIL_JNI_STUB(DefineClass, ZeroWord);
  MOCKTAIL_JNI_STUB(GetModule, ZeroWord);
  MOCKTAIL_JNI_STUB(GetStringCritical, ZeroWord);
  MOCKTAIL_JNI_STUB(ReleaseStringCritical, ZeroWord);

  MOCKTAIL_JNI_STUB_NONVIRTUAL(Object, ZeroWord);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Boolean, ZeroWord);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Byte, ZeroWord);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Char, ZeroWord);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Short, ZeroWord);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Int, ZeroWord);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Long, ZeroWord);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Float, ZeroReal);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Double, ZeroReal);
  MOCKTAIL_JNI_STUB_NONVIRTUAL(Void, ZeroWord);

  MOCKTAIL_JNI_STUB_ARRAY(Boolean);
  MOCKTAIL_JNI_STUB_ARRAY(Char);
  MOCKTAIL_JNI_STUB_ARRAY(Short);
  MOCKTAIL_JNI_STUB_ARRAY(Int);
  MOCKTAIL_JNI_STUB_ARRAY(Long);
  MOCKTAIL_JNI_STUB_ARRAY(Double);
  MOCKTAIL_JNI_STUB(GetFloatArrayRegion, ZeroWord);
}

#undef MOCKTAIL_JNI_STUB_ARRAY
#undef MOCKTAIL_JNI_STUB_NONVIRTUAL
#undef MOCKTAIL_JNI_STUB

}  // namespace

void VM::InitJNIFunctionTables() {
  InstallUnimplementedJniFunctions(native_interface_);

  invoke_interface_.AttachCurrentThread =
      [](JavaVM* vm, void** env, void* args) -> jint {
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] AttachCurrentThread enter vm=" << vm
                << " env_out=" << env << '\n';
    }
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    if (env != nullptr) {
      *env = nullptr;
    }
    VM* owner = VM::FromJavaVM(vm);
    if (!env || !owner) {
      if (JniVmTraceEnabled()) {
        std::cout << "  [JNI] AttachCurrentThread invalid args\n";
      }
      return JNI_EINVAL;
    }
    if (g_thread_vm_instance != owner || !IsThreadLocalEnvValid()) {
      g_thread_audio_fp_mode.Restore();
      g_thread_vm_instance = owner;
      if (!owner->jni_env_) {
        owner->jni_env_ = &owner->jni_env_storage_;
        owner->jni_env_->functions = &owner->native_interface_;
      }
      g_thread_env_storage.functions =
          owner->jni_env_->functions ? owner->jni_env_->functions
                                    : &owner->native_interface_;
      g_thread_local_env = &g_thread_env_storage;
      const auto* attach_args = static_cast<const JavaVMAttachArgs*>(args);
      if (attach_args != nullptr &&
          g_thread_audio_fp_mode.Enable(attach_args->name)) {
        std::fprintf(stderr, "  [mocktail][audio] %s: enabled FTZ/DAZ\n",
                     attach_args->name);
      }
    }
    // Reattaching to the same VM must retain any guest JNI table wrapper.
    *env = g_thread_local_env;
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] AttachCurrentThread return env="
                << g_thread_local_env
                << '\n';
    }
    return JNI_OK;
  };

  invoke_interface_.AttachCurrentThreadAsDaemon =
      invoke_interface_.AttachCurrentThread;

  invoke_interface_.DetachCurrentThread = [](JavaVM* vm) -> jint {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    VM* owner = VM::FromJavaVM(vm);
    if (owner == nullptr) {
      return JNI_EINVAL;
    }
    if (g_thread_vm_instance != owner || !IsThreadLocalEnvValid()) {
      return JNI_EDETACHED;
    }
    ReleaseThreadLocalRefs();
    g_thread_audio_fp_mode.Restore();
    g_thread_local_env = nullptr;
    g_thread_vm_instance = nullptr;
    g_thread_env_storage.functions = nullptr;
    return JNI_OK;
  };

  invoke_interface_.GetEnv =
      [](JavaVM* vm, void** env, jint version) -> jint {
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetEnv enter vm=" << vm << " env_out=" << env
                << " version=" << version << '\n';
    }
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    if (env != nullptr) {
      *env = nullptr;
    }
    VM* owner = VM::FromJavaVM(vm);
    if (!env || !owner) {
      if (JniVmTraceEnabled()) {
        std::cout << "  [JNI] GetEnv invalid args\n";
      }
      return JNI_EINVAL;
    }
    if (g_thread_vm_instance != owner || !IsThreadLocalEnvValid()) {
      if (JniVmTraceEnabled()) {
        std::cout << "  [JNI] GetEnv return detached\n";
      }
      return JNI_EDETACHED;
    }
    *env = g_thread_local_env;
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetEnv return env=" << *env << '\n';
    }
    return JNI_OK;
  };

  invoke_interface_.DestroyJavaVM = [](JavaVM* /*vm*/) -> jint {
    return JNI_OK;
  };

  java_vm_storage_.functions = &invoke_interface_;
  java_vm_ = &java_vm_storage_;

  native_interface_.FindClass =
      [](JNIEnv* /*env*/, const char* name) -> jclass {
    if (JniVmTraceEnabled()) {
      fprintf(stderr, "  [JNI-VM] FindClass name_ptr=%p name=\"%s\" vm=%p\n",
              static_cast<const void*>(name), name ? name : "",
              static_cast<void*>(CurrentVM()));
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] FindClass: " << (name ? name : "null") << '\n';
    }
    auto cls = FallbackClassForName(name ? name : "java/lang/Object");
    return StoreClass(std::move(cls));
  };

  native_interface_.GetVersion = [](JNIEnv* /*env*/) -> jint {
    return JNI_VERSION_1_6;
  };

  native_interface_.NewStringUTF =
      [](JNIEnv* /*env*/, const char* utf) -> jstring {
    return MakeString(utf);
  };

  native_interface_.GetStringUTFLength =
      [](JNIEnv* /*env*/, jstring str) -> jsize {
    return StringModifiedUtf8Length(str);
  };

  native_interface_.GetStringUTFChars =
      [](JNIEnv* /*env*/, jstring str, jboolean* isCopy) -> const char* {
    if (isCopy) *isCopy = JNI_FALSE;
    const char* result = StringChars(str);
    if (StringTraceEnabled()) {
      const std::size_t length =
          static_cast<std::size_t>(StringModifiedUtf8Length(str));
      fprintf(stderr,
              "  [JNI] GetStringUTFChars str=%p chars=%p len=%zu\n",
              static_cast<void*>(str), static_cast<const void*>(result),
              length);
    }
    return result;
  };

  native_interface_.ReleaseStringUTFChars =
      [](JNIEnv* /*env*/, jstring /*str*/, const char* /*chars*/) {};

  native_interface_.ExceptionOccurred = [](JNIEnv* /*env*/) -> jthrowable {
    return nullptr;
  };

  native_interface_.ExceptionDescribe = [](JNIEnv* /*env*/) {};

  native_interface_.ExceptionClear = [](JNIEnv* /*env*/) {};

  native_interface_.ExceptionCheck = [](JNIEnv* /*env*/) -> jboolean {
    return JNI_FALSE;
  };

  native_interface_.Throw = [](JNIEnv* /*env*/, jthrowable /*obj*/) -> jint {
    return JNI_ERR;
  };

  native_interface_.ThrowNew =
      [](JNIEnv* /*env*/, jclass /*clazz*/, const char* msg) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] ThrowNew: " << (msg ? msg : "") << '\n';
    }
    return JNI_ERR;
  };

  native_interface_.FatalError = [](JNIEnv* /*env*/, const char* msg) {
    std::cerr << "[JNI] FatalError: " << (msg ? msg : "") << '\n';
    std::abort();
  };

  native_interface_.PushLocalFrame =
      [](JNIEnv* /*env*/, jint /*capacity*/) -> jint {
    PushLocalJniFrame();
    return JNI_OK;
  };

  native_interface_.PopLocalFrame =
      [](JNIEnv* /*env*/, jobject result) -> jobject {
    return PopLocalJniFrame(result);
  };

  native_interface_.EnsureLocalCapacity =
      [](JNIEnv* /*env*/, jint /*capacity*/) -> jint {
    return JNI_OK;
  };

	  native_interface_.RegisterNatives =
	      [](JNIEnv* /*env*/, jclass clazz, const JNINativeMethod* methods, jint nMethods) -> jint {
	    auto cls = ClassFromJClass(clazz);
	    if (methods == nullptr || nMethods <= 0) {
	      return JNI_OK;
	    }
	    for (int i = 0; i < nMethods; ++i) {
	      const char* name = methods[i].name;
	      if (name == nullptr) {
	        continue;
	      }
	      if (std::strcmp(name, "onStartNative") == 0) {
	        mocktail_gameactivity_on_start_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onResumeNative") == 0) {
	        mocktail_gameactivity_on_resume_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onSurfaceCreatedNative") == 0) {
	        mocktail_gameactivity_on_surface_created_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onSurfaceChangedNative") == 0) {
	        mocktail_gameactivity_on_surface_changed_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onSurfaceRedrawNeededNative") == 0) {
	        mocktail_gameactivity_on_surface_redraw_needed_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onTrimMemoryNative") == 0) {
	        mocktail_gameactivity_on_trim_memory_native = methods[i].fnPtr;
	      }
              if (cls != nullptr &&
                  cls->GetName() ==
                      "org/webrtc/voiceengine/WebRtcAudioManager") {
                if (VM* vm = CurrentVM()) {
                  vm->RegisterWebRtcAudioManagerNative(
                      methods[i].name, methods[i].signature, methods[i].fnPtr);
                }
              }
              if (cls != nullptr &&
                  cls->GetName() ==
                      "org/webrtc/voiceengine/WebRtcAudioRecord") {
                VM *vm = CurrentVM();
                if (vm != nullptr) {
                  vm->RegisterWebRtcAudioRecordNative(
                      methods[i].name, methods[i].signature, methods[i].fnPtr);
                }
              }
              if (cls != nullptr &&
                  cls->GetName() == "org/webrtc/voiceengine/WebRtcAudioTrack") {
                VM *vm = CurrentVM();
                if (vm != nullptr) {
                  vm->RegisterWebRtcAudioTrackNative(
                      methods[i].name, methods[i].signature, methods[i].fnPtr);
                }
              }
            }
	    if (TraceEnabled()) {
	      std::cout << "  [JNI] RegisterNatives for class "
	                << (cls ? cls->GetName() : "unknown") << " ("
                << nMethods << " methods):\n";
      for (int i = 0; i < nMethods; ++i) {
        std::cout << "    " << methods[i].name << " "
                  << methods[i].signature << " -> " << methods[i].fnPtr
                  << '\n';
      }
    }
    return JNI_OK;
  };

  native_interface_.UnregisterNatives =
      [](JNIEnv* /*env*/, jclass /*clazz*/) -> jint {
    return JNI_OK;
  };

  native_interface_.GetStaticMethodID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jmethodID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetStaticMethodID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return StoreMethodId(name, sig);
  };

  native_interface_.GetMethodID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jmethodID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetMethodID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return StoreMethodId(name, sig);
  };

  native_interface_.GetStaticFieldID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jfieldID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetStaticFieldID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return StoreFieldId(name);
  };

  native_interface_.GetFieldID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jfieldID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetFieldID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return StoreFieldId(name);
  };

  native_interface_.AllocObject =
      [](JNIEnv* /*env*/, jclass clazz) -> jobject {
    Trace("AllocObject");
    return MakeObject(clazz);
  };

  native_interface_.NewObject = NewObject;

  native_interface_.NewObjectV = [](JNIEnv * /*env*/, jclass clazz,
                                    jmethodID methodID,
                                    va_list args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] NewObjectV: " << MethodName(methodID) << '\n';
    }
    va_list copy;
    va_copy(copy, args);
    jobject object = ConstructObjectV(clazz, methodID, copy);
    va_end(copy);
    return object;
  };

  native_interface_.NewObjectA = [](JNIEnv * /*env*/, jclass clazz,
                                    jmethodID methodID,
                                    const jvalue *args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] NewObjectA: " << MethodName(methodID) << '\n';
    }
    return ConstructObjectA(clazz, methodID, args);
  };

  native_interface_.GetObjectClass =
      [](JNIEnv* /*env*/, jobject obj) -> jclass {
    Trace("GetObjectClass");
    auto* pseudo_object = PseudoObjectFromRef(obj);
    if (pseudo_object) {
      return StoreClass(pseudo_object->GetClass());
    }
    return StoreClass(FallbackClassForName("java/lang/Object"));
  };

  native_interface_.GetSuperclass =
      [](JNIEnv* /*env*/, jclass /*sub*/) -> jclass {
    return StoreClass(FallbackClassForName("java/lang/Object"));
  };

  native_interface_.IsAssignableFrom =
      [](JNIEnv* /*env*/, jclass sub, jclass sup) -> jboolean {
    return (sub == sup || sup == nullptr) ? JNI_TRUE : JNI_TRUE;
  };

  native_interface_.IsInstanceOf =
      [](JNIEnv* /*env*/, jobject obj, jclass /*clazz*/) -> jboolean {
    return obj ? JNI_TRUE : JNI_FALSE;
  };

  native_interface_.IsSameObject =
      [](JNIEnv* /*env*/, jobject obj1, jobject obj2) -> jboolean {
    return obj1 == obj2 ? JNI_TRUE : JNI_FALSE;
  };

  native_interface_.FromReflectedMethod =
      [](JNIEnv* /*env*/, jobject method) -> jmethodID {
    return reinterpret_cast<jmethodID>(method);
  };

  native_interface_.FromReflectedField =
      [](JNIEnv* /*env*/, jobject field) -> jfieldID {
    return reinterpret_cast<jfieldID>(field);
  };

  native_interface_.ToReflectedMethod =
      [](JNIEnv* /*env*/, jclass /*cls*/, jmethodID methodID,
         jboolean /*isStatic*/) -> jobject {
    return reinterpret_cast<jobject>(methodID);
  };

  native_interface_.ToReflectedField =
      [](JNIEnv* /*env*/, jclass /*cls*/, jfieldID fieldID,
         jboolean /*isStatic*/) -> jobject {
    return reinterpret_cast<jobject>(fieldID);
  };

  native_interface_.CallStaticVoidMethod = CallStaticVoidMethod;

  native_interface_.CallStaticVoidMethodV =
      [](JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticVoidMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    HandleStaticVoidMethodV(env, clazz, methodID, args);
  };

  native_interface_.CallStaticVoidMethodA =
      [](JNIEnv* env, jclass clazz, jmethodID methodID,
         const jvalue* args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticVoidMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    HandleStaticVoidMethodA(env, clazz, methodID, args);
  };

  native_interface_.CallStaticObjectMethod = CallStaticObjectMethod;

  native_interface_.CallStaticObjectMethodV = [](JNIEnv * /*env*/, jclass clazz,
                                                 jmethodID methodID,
                                                 va_list args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticObjectMethodV: " << MethodName(methodID)
                << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    jobject result = ObjectResultForMethodV(nullptr, methodID, args);
    if (result != nullptr) {
      return result;
    }
    result = ExactMessageBusStaticObject(clazz, methodID);
    return result != nullptr ? result : StaticObjectResultForMethod(methodID);
  };

  native_interface_.CallStaticObjectMethodA =
      [](JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
         const jvalue *args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticObjectMethodA: " << MethodName(methodID)
                << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    const JniMethod method = MethodOf(methodID);
    if (args && method == JniMethod::kForName) {
      return ClassObjectForName(
          StringFromJString(reinterpret_cast<jstring>(args[0].l)));
    }
    jobject cookie_result = nullptr;
    if (CookieObjectResultForMethodA(methodID, args, &cookie_result)) {
      return cookie_result;
    }
    jobject message_bus = ExactMessageBusStaticObject(clazz, methodID);
    if (message_bus != nullptr) {
      return message_bus;
    }
    return StaticObjectResultForMethod(methodID);
  };

  native_interface_.CallStaticBooleanMethod = CallStaticBooleanMethod;

  native_interface_.CallStaticBooleanMethodV =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID, va_list /*args*/) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticBooleanMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    return StaticBooleanResultForMethod(methodID);
  };

  native_interface_.CallStaticBooleanMethodA =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         const jvalue* /*args*/) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticBooleanMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    return StaticBooleanResultForMethod(methodID);
  };

  native_interface_.CallStaticByteMethod = CallStaticByteMethod;
  native_interface_.CallStaticByteMethodV =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         va_list /*args*/) -> jbyte {
    ReportUnhandledCall(clazz, methodID, true);
    return 0;
  };
  native_interface_.CallStaticByteMethodA =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         const jvalue* /*args*/) -> jbyte {
    ReportUnhandledCall(clazz, methodID, true);
    return 0;
  };

  native_interface_.CallStaticCharMethod = CallStaticCharMethod;
  native_interface_.CallStaticCharMethodV =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         va_list /*args*/) -> jchar {
    ReportUnhandledCall(clazz, methodID, true);
    return 0;
  };
  native_interface_.CallStaticCharMethodA =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         const jvalue* /*args*/) -> jchar {
    ReportUnhandledCall(clazz, methodID, true);
    return 0;
  };

  native_interface_.CallStaticShortMethod = CallStaticShortMethod;
  native_interface_.CallStaticShortMethodV =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         va_list /*args*/) -> jshort {
    ReportUnhandledCall(clazz, methodID, true);
    return 0;
  };
  native_interface_.CallStaticShortMethodA =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         const jvalue* /*args*/) -> jshort {
    ReportUnhandledCall(clazz, methodID, true);
    return 0;
  };

  native_interface_.CallStaticIntMethod = CallStaticIntMethod;

  native_interface_.CallStaticIntMethodV =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID, va_list args) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticIntMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    return StaticIntResultForMethodV(methodID, args);
  };

  native_interface_.CallStaticIntMethodA =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID,
         const jvalue* args) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticIntMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    return StaticIntResultForMethodA(methodID, args);
  };

  native_interface_.CallStaticLongMethod = CallStaticLongMethod;

  native_interface_.CallStaticLongMethodV =
      [](JNIEnv* /*env*/, jclass clazz, jmethodID methodID, va_list /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticLongMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    return StaticLongResultForMethod(methodID);
  };

  native_interface_.CallStaticLongMethodA =
      [](JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
         const jvalue * /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticLongMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(clazz, methodID, true);
    return StaticLongResultForMethod(methodID);
  };

  native_interface_.CallStaticFloatMethod = CallStaticFloatMethod;
  native_interface_.CallStaticFloatMethodV =
      [](JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
         va_list /*args*/) -> jfloat {
    ReportUnhandledCall(clazz, methodID, true);
    return 0.0f;
  };
  native_interface_.CallStaticFloatMethodA =
      [](JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
         const jvalue * /*args*/) -> jfloat {
    ReportUnhandledCall(clazz, methodID, true);
    return 0.0f;
  };

  native_interface_.CallStaticDoubleMethod = CallStaticDoubleMethod;
  native_interface_.CallStaticDoubleMethodV =
      [](JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
         va_list /*args*/) -> jdouble {
    ReportUnhandledCall(clazz, methodID, true);
    return 0.0;
  };
  native_interface_.CallStaticDoubleMethodA =
      [](JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
         const jvalue * /*args*/) -> jdouble {
    ReportUnhandledCall(clazz, methodID, true);
    return 0.0;
  };

  native_interface_.CallVoidMethod = CallVoidMethod;

  native_interface_.CallVoidMethodV = [](JNIEnv * /*env*/, jobject obj,
                                         jmethodID methodID, va_list args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallVoidMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    if (!HandleRobloxExperienceLifecycleVoidMethod(obj, methodID) &&
        !HandleWebRtcAudioManagerVoidMethodV(obj, methodID, args) &&
        !HandleRobloxOpenWebActivityMethodV(obj, methodID, args) &&
        !HandleRobloxTextInputInstanceVoidMethodV(obj, methodID, args) &&
        !HandleFmodAudioDeviceVoidMethodV(obj, methodID, args) &&
        !HandleRobloxCookieSetVoidMethodV(obj, methodID, args) &&
        !HandleMemStorageCallbackVoidMethodV(obj, methodID, args)) {
      HandleVoidMethod(obj, methodID, args);
    }
  };

  native_interface_.CallVoidMethodA = [](JNIEnv * /*env*/, jobject obj,
                                         jmethodID methodID,
                                         const jvalue *args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallVoidMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    if (!HandleRobloxExperienceLifecycleVoidMethod(obj, methodID) &&
        !HandleWebRtcAudioManagerVoidMethodA(obj, methodID, args) &&
        !HandleRobloxOpenWebActivityMethodA(obj, methodID, args) &&
        !HandleRobloxTextInputInstanceVoidMethodA(obj, methodID, args) &&
        !HandleFmodAudioDeviceVoidMethodA(obj, methodID, args) &&
        !HandleRobloxCookieSetVoidMethodA(obj, methodID, args) &&
        !HandleMemStorageCallbackVoidMethodA(obj, methodID, args)) {
      HandleVoidMethodA(obj, methodID, args);
    }
  };

  native_interface_.CallObjectMethod = CallObjectMethod;

  native_interface_.CallObjectMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallObjectMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    return ObjectResultForMethodV(obj, methodID, args);
  };

  native_interface_.CallObjectMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallObjectMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    const JniMethod method = MethodOf(methodID);
    if (IsJavaStringGetBytesMethod(obj, methodID)) {
      return args != nullptr
                 ? JavaStringGetUtf8Bytes(
                       obj, static_cast<jstring>(args[0].l))
                 : nullptr;
    }
    if (args && method == JniMethod::kRun &&
        ObjectClassName(obj) ==
            "com/roblox/universalapp/messagebus/RequestHandlerRaw") {
      VM* vm = CurrentVM();
      return vm != nullptr
                 ? vm->DispatchMessageBusRequestHandler(
                       obj, vm->GetJNIEnv(), static_cast<jstring>(args[0].l))
                 : nullptr;
    }
    jobject local_storage_result = nullptr;
    if (LocalStorageObjectResultForMethodA(methodID, args, &local_storage_result)) {
      return local_storage_result;
    }
    jobject cookie_result = nullptr;
    if (CookieObjectResultForMethodA(methodID, args, &cookie_result)) {
      return cookie_result;
    }
    if (args && method == JniMethod::kGetSystemService) {
      return SystemServiceObject(StringFromJString(
          reinterpret_cast<jstring>(args[0].l)));
    }
    if (args && (method == JniMethod::kLoadClass ||
                 method == JniMethod::kFindClass)) {
      return ClassObjectForName(StringFromJString(
          reinterpret_cast<jstring>(args[0].l)));
    }
    if (args && method == JniMethod::kGetString) {
      return args[1].l ? args[1].l : MakeString("");
    }
    jobject receiver_result = ObjectResultForReceiverMethod(obj, methodID);
    jobject result =
        receiver_result ? receiver_result : ObjectResultForMethod(methodID);
    if (result == nullptr) {
      MarkCallUnhandled();
    }
    return result;
  };

  native_interface_.CallBooleanMethod = CallBooleanMethod;

  native_interface_.CallBooleanMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list args) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallBooleanMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    jboolean result = JNI_FALSE;
    if (HandleWebRtcAudioManagerBooleanMethod(obj, methodID, &result)) {
      return result;
    }
    if (HandleWebRtcAudioRecordBooleanMethodV(obj, methodID, args, &result)) {
      return result;
    }
    if (HandleWebRtcAudioTrackBooleanMethodV(obj, methodID, args, &result)) {
      return result;
    }
    if (HandleFmodAudioDeviceBooleanMethodV(obj, methodID, args, &result)) {
      return result;
    }
    if (PackageManagerBooleanResultForMethodV(obj, methodID, args, &result)) {
      return result;
    }
    if (LocalStorageBooleanResultForMethodV(methodID, args,
                                            &result)) {
      return result;
    }
    if (CookieBooleanResultForMethod(methodID, &result)) {
      return result;
    }
    return BooleanResultForReceiverMethod(obj, methodID);
  };

  native_interface_.CallBooleanMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* args) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallBooleanMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    jboolean result = JNI_FALSE;
    if (HandleWebRtcAudioManagerBooleanMethod(obj, methodID, &result)) {
      return result;
    }
    if (HandleWebRtcAudioRecordBooleanMethodA(obj, methodID, args, &result)) {
      return result;
    }
    if (HandleWebRtcAudioTrackBooleanMethodA(obj, methodID, args, &result)) {
      return result;
    }
    if (HandleFmodAudioDeviceBooleanMethodA(obj, methodID, args, &result)) {
      return result;
    }
    if (PackageManagerBooleanResultForMethodA(obj, methodID, args, &result)) {
      return result;
    }
    if (LocalStorageBooleanResultForMethodA(methodID, args,
                                            &result)) {
      return result;
    }
    if (CookieBooleanResultForMethod(methodID, &result)) {
      return result;
    }
    return BooleanResultForReceiverMethod(obj, methodID);
  };

  native_interface_.CallByteMethod = CallByteMethod;
  native_interface_.CallByteMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list /*args*/) -> jbyte {
    ReportUnhandledCall(obj, methodID, false);
    return 0;
  };
  native_interface_.CallByteMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jbyte {
    ReportUnhandledCall(obj, methodID, false);
    return 0;
  };

  native_interface_.CallCharMethod = CallCharMethod;
  native_interface_.CallCharMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list /*args*/) -> jchar {
    ReportUnhandledCall(obj, methodID, false);
    return 0;
  };
  native_interface_.CallCharMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jchar {
    ReportUnhandledCall(obj, methodID, false);
    return 0;
  };

  native_interface_.CallShortMethod = CallShortMethod;
  native_interface_.CallShortMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list /*args*/) -> jshort {
    ReportUnhandledCall(obj, methodID, false);
    return 0;
  };
  native_interface_.CallShortMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jshort {
    ReportUnhandledCall(obj, methodID, false);
    return 0;
  };

  native_interface_.CallIntMethod = CallIntMethod;

  native_interface_.CallIntMethodV = [](JNIEnv * /*env*/, jobject obj,
                                        jmethodID methodID,
                                        va_list args) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallIntMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    jint result = 0;
    if (HandleWebRtcAudioRecordIntMethodV(obj, methodID, args, &result) ||
        HandleWebRtcAudioTrackIntMethodV(obj, methodID, args, &result)) {
      return result;
    }
    return IntResultForReceiverMethod(obj, methodID);
  };

  native_interface_.CallIntMethodA = [](JNIEnv * /*env*/, jobject obj,
                                        jmethodID methodID,
                                        const jvalue *args) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallIntMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    jint result = 0;
    if (HandleWebRtcAudioRecordIntMethodA(obj, methodID, args, &result) ||
        HandleWebRtcAudioTrackIntMethodA(obj, methodID, args, &result)) {
      return result;
    }
    return IntResultForReceiverMethod(obj, methodID);
  };

  native_interface_.CallLongMethod = CallLongMethod;

  native_interface_.CallLongMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID, va_list /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallLongMethodV: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    jlong result = 0;
    if (LocalStorageLongResultForMethod(methodID, &result)) {
      return result;
    }
    return LongResultForReceiverMethod(obj, methodID);
  };

  native_interface_.CallLongMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallLongMethodA: " << MethodName(methodID) << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    jlong result = 0;
    if (LocalStorageLongResultForMethod(methodID, &result)) {
      return result;
    }
    return LongResultForReceiverMethod(obj, methodID);
  };

  native_interface_.CallFloatMethod = CallFloatMethod;
  native_interface_.CallFloatMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list /*args*/) -> jfloat {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallFloatMethodV: " << MethodName(methodID)
                << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    return FloatResultForReceiverMethod(obj, methodID);
  };
  native_interface_.CallFloatMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jfloat {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallFloatMethodA: " << MethodName(methodID)
                << '\n';
    }
    const UnhandledCallScope unhandled_call(obj, methodID, false);
    return FloatResultForReceiverMethod(obj, methodID);
  };

  native_interface_.CallDoubleMethod = CallDoubleMethod;
  native_interface_.CallDoubleMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list /*args*/) -> jdouble {
    ReportUnhandledCall(obj, methodID, false);
    return 0.0;
  };
  native_interface_.CallDoubleMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jdouble {
    ReportUnhandledCall(obj, methodID, false);
    return 0.0;
  };

  native_interface_.GetStaticObjectField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID fieldID) -> jobject {
    if (TraceEnabled()) {
      auto* name = reinterpret_cast<const char*>(fieldID);
      std::cout << "  [JNI] GetStaticObjectField: "
                << (name ? name : "unknown") << '\n';
    }
    auto* name = reinterpret_cast<const char*>(fieldID);
    if (name && std::strcmp(name, "ANDROID_ID") == 0) {
      return MakeString("mocktail-android-id");
    }
    if (name && std::strcmp(name, "INSTANCE") == 0) {
      return MakePlatformSystemDialogHandlerObject();
    }
    if (name && std::strcmp(name, "sImplementation") == 0) {
      return EngineJavaCallbackObject();
    }
    return StaticObjectFieldValue(name);
  };

  native_interface_.GetStaticBooleanField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jboolean {
    return JNI_FALSE;
  };
  native_interface_.GetStaticByteField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jbyte {
    return 0;
  };
  native_interface_.GetStaticCharField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jchar {
    return 0;
  };
  native_interface_.GetStaticShortField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jshort {
    return 0;
  };
  native_interface_.GetStaticIntField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jint {
    return 0;
  };
  native_interface_.GetStaticLongField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jlong {
    return 0;
  };
  native_interface_.GetStaticFloatField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jfloat {
    return 0.0f;
  };
  native_interface_.GetStaticDoubleField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jdouble {
    return 0.0;
  };

  native_interface_.SetStaticObjectField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID fieldID,
         jobject value) {
    SetStaticObjectFieldRaw(reinterpret_cast<const char*>(fieldID), value);
  };
  native_interface_.SetStaticBooleanField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jboolean /*value*/) {};
  native_interface_.SetStaticByteField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jbyte /*value*/) {};
  native_interface_.SetStaticCharField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jchar /*value*/) {};
  native_interface_.SetStaticShortField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jshort /*value*/) {};
  native_interface_.SetStaticIntField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jint /*value*/) {};
  native_interface_.SetStaticLongField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jlong /*value*/) {};
  native_interface_.SetStaticFloatField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jfloat /*value*/) {};
  native_interface_.SetStaticDoubleField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jdouble /*value*/) {};

  native_interface_.GetObjectField =
      [](JNIEnv* env, jobject obj, jfieldID fieldID) -> jobject {
    auto* name = reinterpret_cast<const char*>(fieldID);
    jobject value = ObjectFieldValue(obj, name);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetObjectField: "
                << (name ? name : "unknown") << " -> " << value << '\n';
    }
    return value;
  };
  native_interface_.GetBooleanField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jboolean {
    return BooleanFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetByteField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jbyte {
    return 0;
  };
  native_interface_.GetCharField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jchar {
    return 0;
  };
  native_interface_.GetShortField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jshort {
    return 0;
  };
  native_interface_.GetIntField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jint {
    return IntFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetLongField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jlong {
    return LongFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetFloatField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jfloat {
    return FloatFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetDoubleField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jdouble {
    return 0.0;
  };

  native_interface_.SetObjectField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID,
         jobject val) {
    SetObjectFieldRaw(obj, reinterpret_cast<const char*>(fieldID), val);
  };
  native_interface_.SetBooleanField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID,
         jboolean val) {
    SetBooleanFieldRaw(obj, reinterpret_cast<const char*>(fieldID), val);
  };
  native_interface_.SetByteField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jbyte /*val*/) {};
  native_interface_.SetCharField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jchar /*val*/) {};
  native_interface_.SetShortField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jshort /*val*/) {};
  native_interface_.SetIntField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID, jint val) {
    SetIntFieldRaw(obj, reinterpret_cast<const char*>(fieldID), val);
  };
  native_interface_.SetLongField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID, jlong val) {
    SetLongFieldRaw(obj, reinterpret_cast<const char*>(fieldID), val);
  };
  native_interface_.SetFloatField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID, jfloat val) {
    SetFloatFieldRaw(obj, reinterpret_cast<const char*>(fieldID), val);
  };
  native_interface_.SetDoubleField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jdouble /*val*/) {};

  native_interface_.NewGlobalRef =
      [](JNIEnv* /*env*/, jobject obj) -> jobject {
    RetainJniReference(obj);
    return obj;
  };

  native_interface_.DeleteGlobalRef =
      [](JNIEnv* /*env*/, jobject obj) {
    ReleaseJniReference(obj);
  };

  native_interface_.NewLocalRef =
      [](JNIEnv* /*env*/, jobject obj) -> jobject {
    if (obj != nullptr) {
      RetainJniReference(obj);
      RegisterLocalRef(obj);
    }
    return obj;
  };

  native_interface_.DeleteLocalRef =
      [](JNIEnv* /*env*/, jobject obj) {
    UnregisterLocalRef(obj);
    ReleaseJniReference(obj);
  };

  native_interface_.NewWeakGlobalRef =
      [](JNIEnv* /*env*/, jobject obj) -> jweak {
    Trace("NewWeakGlobalRef");
    RetainJniReference(obj);
    return reinterpret_cast<jweak>(obj);
  };

  native_interface_.DeleteWeakGlobalRef =
      [](JNIEnv* /*env*/, jweak ref) {
    ReleaseJniReference(ref);
  };

  native_interface_.GetObjectRefType =
      [](JNIEnv* /*env*/, jobject obj) -> jobjectRefType {
    return obj ? JNILocalRefType : JNIInvalidRefType;
  };

  native_interface_.NewString =
      [](JNIEnv* /*env*/, const jchar* unicode, jsize len) -> jstring {
    return MakeUtf16String(unicode, len);
  };

  native_interface_.GetStringLength =
      [](JNIEnv* /*env*/, jstring str) -> jsize {
    return StringUtf16Length(str);
  };

  native_interface_.GetStringChars =
      [](JNIEnv* /*env*/, jstring str, jboolean* isCopy) -> const jchar* {
    if (isCopy) *isCopy = JNI_FALSE;
    return StringUtf16Chars(str);
  };

  native_interface_.ReleaseStringChars =
      [](JNIEnv* /*env*/, jstring /*str*/, const jchar* /*chars*/) {};

  native_interface_.GetStringUTFRegion =
      [](JNIEnv* /*env*/, jstring str, jsize start, jsize len, char* buf) {
    CopyStringModifiedUtf8Region(str, start, len, buf);
  };

  native_interface_.GetStringRegion =
      [](JNIEnv* /*env*/, jstring str, jsize start, jsize len, jchar* buf) {
    CopyStringRegion(str, start, len, buf);
  };

  native_interface_.GetArrayLength =
      [](JNIEnv* /*env*/, jarray array) -> jsize {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array) {
      return 0;
    }
    if (!pseudo_array->bytes.empty()) {
      return static_cast<jsize>(pseudo_array->bytes.size());
    }
    if (!pseudo_array->floats.empty()) {
      return static_cast<jsize>(pseudo_array->floats.size());
    }
    return static_cast<jsize>(pseudo_array->objects.size());
  };

  native_interface_.NewObjectArray =
      [](JNIEnv* /*env*/, jsize len, jclass /*clazz*/,
         jobject init) -> jobjectArray {
    Trace("NewObjectArray");
    return MakeObjectArray(len, init);
  };

  native_interface_.GetObjectArrayElement =
      [](JNIEnv* env, jobjectArray array, jsize index) -> jobject {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || index < 0 ||
        static_cast<std::size_t>(index) >= pseudo_array->objects.size()) {
      return nullptr;
    }
    jobject obj = pseudo_array->objects[static_cast<std::size_t>(index)];
    return env != nullptr && obj != nullptr ? env->NewLocalRef(obj) : obj;
  };

  native_interface_.SetObjectArrayElement =
      [](JNIEnv* env, jobjectArray array, jsize index, jobject val) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || index < 0 ||
        static_cast<std::size_t>(index) >= pseudo_array->objects.size()) {
      return;
    }
    if (val != nullptr) {
      RetainJniReference(val);
    }
    jobject prev = pseudo_array->objects[static_cast<std::size_t>(index)];
    pseudo_array->objects[static_cast<std::size_t>(index)] = val;
    if (prev != nullptr) {
      ReleaseJniReference(prev);
    }
  };

  native_interface_.NewByteArray =
      [](JNIEnv* /*env*/, jsize len) -> jbyteArray {
    Trace("NewByteArray");
    return MakeByteArray(len);
  };

  native_interface_.GetByteArrayElements =
      [](JNIEnv* /*env*/, jbyteArray array, jboolean* isCopy) -> jbyte* {
    if (isCopy) *isCopy = JNI_FALSE;
    PseudoArray* pseudo_array = ArrayFromRef(array);
    return pseudo_array && !pseudo_array->bytes.empty()
               ? pseudo_array->bytes.data()
               : nullptr;
  };

  native_interface_.ReleaseByteArrayElements =
      [](JNIEnv* /*env*/, jbyteArray /*array*/, jbyte* /*elems*/,
         jint /*mode*/) {};

  native_interface_.NewFloatArray =
      [](JNIEnv* /*env*/, jsize len) -> jfloatArray {
    Trace("NewFloatArray");
    return MakeFloatArray(len);
  };

  native_interface_.GetFloatArrayElements =
      [](JNIEnv* /*env*/, jfloatArray array, jboolean* isCopy) -> jfloat* {
    if (isCopy) *isCopy = JNI_FALSE;
    PseudoArray* pseudo_array = ArrayFromRef(array);
    return pseudo_array && !pseudo_array->floats.empty()
               ? pseudo_array->floats.data()
               : nullptr;
  };

  native_interface_.ReleaseFloatArrayElements =
      [](JNIEnv* /*env*/, jfloatArray /*array*/, jfloat* /*elems*/,
         jint /*mode*/) {};

  native_interface_.GetByteArrayRegion =
      [](JNIEnv* /*env*/, jbyteArray array, jsize start, jsize len, jbyte* buf) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || !buf || start < 0 || len <= 0) {
      return;
    }
    std::size_t offset = static_cast<std::size_t>(start);
    if (offset >= pseudo_array->bytes.size()) {
      return;
    }
    std::size_t count =
        std::min(static_cast<std::size_t>(len),
                 pseudo_array->bytes.size() - offset);
    std::memcpy(buf, pseudo_array->bytes.data() + offset, count);
  };

  native_interface_.SetByteArrayRegion =
      [](JNIEnv* /*env*/, jbyteArray array, jsize start, jsize len,
         const jbyte* buf) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || !buf || start < 0 || len <= 0) {
      return;
    }
    std::size_t offset = static_cast<std::size_t>(start);
    if (offset >= pseudo_array->bytes.size()) {
      return;
    }
    std::size_t count =
        std::min(static_cast<std::size_t>(len),
                 pseudo_array->bytes.size() - offset);
    std::memcpy(pseudo_array->bytes.data() + offset, buf, count);
  };

  native_interface_.SetFloatArrayRegion =
      [](JNIEnv* /*env*/, jfloatArray array, jsize start, jsize len,
         const jfloat* buf) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || !buf || start < 0 || len <= 0) {
      return;
    }
    std::size_t offset = static_cast<std::size_t>(start);
    if (offset >= pseudo_array->floats.size()) {
      return;
    }
    std::size_t count =
        std::min(static_cast<std::size_t>(len),
                 pseudo_array->floats.size() - offset);
    std::memcpy(pseudo_array->floats.data() + offset, buf,
                count * sizeof(jfloat));
  };

  native_interface_.GetPrimitiveArrayCritical =
      [](JNIEnv* env, jarray array, jboolean* isCopy) -> void* {
    if (isCopy) *isCopy = JNI_FALSE;
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array) {
      return nullptr;
    }
    if (!pseudo_array->bytes.empty()) {
      return pseudo_array->bytes.data();
    }
    return !pseudo_array->floats.empty() ? pseudo_array->floats.data()
                                         : nullptr;
  };

  native_interface_.ReleasePrimitiveArrayCritical =
      [](JNIEnv* /*env*/, jarray /*array*/, void* /*carray*/, jint /*mode*/) {};

  native_interface_.NewDirectByteBuffer = [](JNIEnv * /*env*/, void *address,
                                             jlong capacity) -> jobject {
    if (address == nullptr || capacity < 0) {
      return nullptr;
    }
    const jobject buffer = reinterpret_cast<jobject>(address);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_direct_buffer_capacities[buffer] = capacity;
    return buffer;
  };

  native_interface_.GetDirectBufferAddress =
      [](JNIEnv* /*env*/, jobject buf) -> void* {
    return reinterpret_cast<void*>(buf);
  };

  native_interface_.GetDirectBufferCapacity = [](JNIEnv * /*env*/,
                                                 jobject buf) -> jlong {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    const auto found = g_direct_buffer_capacities.find(buf);
    return found != g_direct_buffer_capacities.end() ? found->second : -1;
  };

  native_interface_.MonitorEnter =
      [](JNIEnv* /*env*/, jobject /*obj*/) -> jint {
    return JNI_OK;
  };

  native_interface_.MonitorExit =
      [](JNIEnv* /*env*/, jobject /*obj*/) -> jint {
    return JNI_OK;
  };

  native_interface_.GetJavaVM =
      [](JNIEnv* env, JavaVM** vm) -> jint {
    VM* current_vm = CurrentVM();
    if (!vm || !current_vm) {
      return JNI_ERR;
    }
    *vm = current_vm->GetJavaVM();
    if (TraceEnabled() || JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetJavaVM env=" << env << " out=" << vm
                << " vm=" << *vm << '\n';
    }
    return JNI_OK;
  };

  jni_env_ = &jni_env_storage_;
  jni_env_->functions = &native_interface_;
}

}  // namespace jnivm
