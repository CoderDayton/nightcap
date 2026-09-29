#include "runtime/roblox_account_bridge.h"

#include <array>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kAccountProtocolClass[] =
    "com/roblox/universalapp/account/JNIAccountProtocol";
// Serialized name of the APK status enum for a missing Play token provider.
constexpr char kTokenProviderUninitialized[] = "TOKEN_PROVIDER_UNINITIALIZED";
constexpr std::size_t kMaximumNameBytes = 128;

enum Method : std::size_t { kDeviceIntegrityAvailable, kGetIntegrityToken };
constexpr std::size_t kMethodCount = 2;
constexpr const char* kMethodLabels[kMethodCount] = {"DeviceIntegrityAvailable",
                                                     "GetIntegrityToken"};

Status Unavailable(const char* message) {
  return Status::Error(StatusCode::kUnavailable, message);
}

Status CheckJni(JNIEnv* env) {
  if (!env->ExceptionCheck()) return Status::Ok();
  env->ExceptionClear();
  return Unavailable("AccountProtocol JNI operation failed");
}

// Copies a non-empty, bounded engine-provided name.
bool ReadName(JNIEnv* env, jstring value, std::string* name) {
  if (!value) return false;
  const jsize size = env->GetStringUTFLength(value);
  const char* bytes = env->GetStringUTFChars(value, nullptr);
  const bool valid = bytes && size > 0 &&
                     static_cast<std::size_t>(size) <= kMaximumNameBytes;
  if (valid) name->assign(bytes, static_cast<std::size_t>(size));
  if (bytes) env->ReleaseStringUTFChars(value, bytes);
  return valid;
}

// Pins a valid engine-provided name as a global reference.
jstring RetainName(JNIEnv* env, jstring value) {
  std::string ignored;
  return ReadName(env, value, &ignored)
             ? static_cast<jstring>(env->NewGlobalRef(value))
             : nullptr;
}

}  // namespace

struct RobloxAccountBridge::State {
  struct Endpoint {
    std::shared_ptr<State> state;
    Method method;
  };
  struct Registration {
    jobject handler = nullptr;
    jstring method = nullptr;
    bool registered = false;
  };

  jobject bus = nullptr;
  jstring protocol = nullptr;
  std::string support_key;
  std::string token_key;
  std::string result_key;
  std::array<Registration, kMethodCount> registrations;
  std::mutex mutex;
  std::condition_variable drained;
  bool accepting = true;
  std::size_t in_flight = 0;

  // The request payload is never parsed or logged: both answers are fixed.
  static std::string Answer(void* context, JNIEnv*, jstring) {
    const auto* endpoint = static_cast<Endpoint*>(context);
    const auto state = endpoint->state;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->accepting) return {};
      ++state->in_flight;
    }
    struct Finish {
      std::shared_ptr<State> state;
      ~Finish() {
        std::lock_guard<std::mutex> lock(state->mutex);
        --state->in_flight;
        state->drained.notify_all();
      }
    } finish{state};

    nlohmann::json body;
    const char* result = nullptr;
    if (endpoint->method == kDeviceIntegrityAvailable) {
      body = {{state->support_key, false}};
      result = "unsupported";
    } else {
      body = {{state->token_key, ""},
              {state->result_key, kTokenProviderUninitialized}};
      result = kTokenProviderUninitialized;
    }
    std::fprintf(stderr, "  [account] AccountProtocol method=%s result=%s\n",
                 kMethodLabels[endpoint->method], result);
    return body.dump();
  }
};

RobloxAccountBridge::RobloxAccountBridge(JniEnvironmentProvider environment,
                                         RobloxAccountProtocolSymbols symbols,
                                         RobloxAccountProtocolObjects objects)
    : environment_(environment), symbols_(symbols), objects_(objects) {}

RobloxAccountBridge::~RobloxAccountBridge() { (void)Shutdown(); }

Status RobloxAccountBridge::Initialize() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (state_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "AccountProtocol is already initialized");
  }
  if (!environment_.valid() || !symbols_.complete() || !objects_.complete()) {
    return Unavailable("AccountProtocol prerequisites are incomplete");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) return status;

  jclass protocol_class = env->FindClass(kAccountProtocolClass);
  if (!protocol_class) {
    (void)CheckJni(env);
    return Unavailable("JNIAccountProtocol class is unavailable");
  }
  const jstring protocol = symbols_.get_protocol_name(env, protocol_class);
  const jstring methods[kMethodCount] = {
      symbols_.get_device_integrity_available_method_name(env, protocol_class),
      symbols_.get_integrity_token_method_name(env, protocol_class)};
  const jstring keys[] = {symbols_.get_support_key(env, protocol_class),
                          symbols_.get_token_key(env, protocol_class),
                          symbols_.get_result_key(env, protocol_class)};
  env->DeleteLocalRef(protocol_class);

  state_ = std::make_shared<State>();
  std::string* const key_targets[] = {&state_->support_key,
                                      &state_->token_key, &state_->result_key};
  bool names_valid = true;
  for (std::size_t i = 0; i < std::size(keys); ++i) {
    names_valid = ReadName(env, keys[i], key_targets[i]) && names_valid;
    if (keys[i]) env->DeleteLocalRef(keys[i]);
  }
  state_->protocol = RetainName(env, protocol);
  if (protocol) env->DeleteLocalRef(protocol);
  for (std::size_t i = 0; i < kMethodCount; ++i) {
    state_->registrations[i].method = RetainName(env, methods[i]);
    names_valid = state_->registrations[i].method && names_valid;
    if (methods[i]) env->DeleteLocalRef(methods[i]);
  }
  state_->bus = env->NewGlobalRef(objects_.message_bus);
  auto fail = [this](Status failure) {
    (void)ShutdownLocked();
    return failure;
  };
  if (!CheckJni(env).ok() || !names_valid || !state_->protocol ||
      !state_->bus) {
    return fail(Unavailable("AccountProtocol names are unavailable"));
  }

  for (std::size_t i = 0; i < kMethodCount; ++i) {
    auto& entry = state_->registrations[i];
    auto endpoint = std::make_shared<State::Endpoint>(
        State::Endpoint{state_, static_cast<Method>(i)});
    jobject handler = objects_.create_request_handler(objects_.context,
                                                      endpoint, &State::Answer);
    if (handler) {
      entry.handler = env->NewGlobalRef(handler);
      if (!entry.handler) {
        objects_.clear_request_handler(objects_.context, handler);
      }
      env->DeleteLocalRef(handler);
    }
    if (!entry.handler || !CheckJni(env).ok()) {
      return fail(Unavailable("could not create AccountProtocol handler"));
    }
    entry.registered = true;
    symbols_.set_request_handler_raw(env, state_->bus, state_->protocol,
                                     entry.method, entry.handler);
    status = CheckJni(env);
    if (!status.ok()) return fail(status);
  }
  std::fprintf(stderr,
               "  [account] AccountProtocol ready device_integrity=unsupported\n");
  return Status::Ok();
}

Status RobloxAccountBridge::Shutdown() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  return ShutdownLocked();
}

Status RobloxAccountBridge::ShutdownLocked() {
  if (!state_) return Status::Ok();
  {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->accepting = false;
    state_->drained.wait(lock, [this] { return state_->in_flight == 0; });
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) return status;  // Keep roots for a later shutdown retry.
  // Clear any exception from partially failed initialization before cleanup.
  const Status pending = CheckJni(env);
  if (!pending.ok()) status = pending;
  for (auto& entry : state_->registrations) {
    if (entry.registered) {
      symbols_.clear_request_handler(env, state_->bus, state_->protocol,
                                     entry.method);
      const Status cleared = CheckJni(env);
      if (!cleared.ok()) status = cleared;
    }
    if (entry.handler) {
      objects_.clear_request_handler(objects_.context, entry.handler);
      env->DeleteGlobalRef(entry.handler);
    }
    if (entry.method) env->DeleteGlobalRef(entry.method);
    entry = {};
  }
  if (state_->protocol) env->DeleteGlobalRef(state_->protocol);
  if (state_->bus) env->DeleteGlobalRef(state_->bus);
  state_.reset();
  return status;
}

}  // namespace runtime
}  // namespace mocktail
