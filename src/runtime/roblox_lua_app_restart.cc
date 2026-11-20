#include "runtime/roblox_lua_app_restart.h"

#include <string>

namespace mocktail {
namespace runtime {
namespace {

Status CheckJni(JNIEnv* env, const char* operation) {
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  return Status::Error(StatusCode::kPlatformError,
                       std::string(operation) + " failed");
}

}  // namespace

Status RestartLuaApp(JNIEnv* env, jclass native_gl_class,
                     const LuaAppRestartSymbols& symbols,
                     jobject start_app_params) {
  jstring stop_reason = env->NewStringUTF("ASMA.stop");
  jstring start_reason = env->NewStringUTF("ASMA.start");

  Status status = Status::Ok();
  if (stop_reason == nullptr || start_reason == nullptr) {
    status = Status::Error(StatusCode::kUnavailable,
                           "could not allocate ASMA surface lifecycle reason");
  }
  if (status.ok()) {
    symbols.pause_app(env, native_gl_class);
    status = CheckJni(env, "pause LuaApp for surface recreation");
  }
  if (status.ok()) {
    symbols.set_task_scheduler_background_mode(env, native_gl_class, JNI_TRUE,
                                               stop_reason);
    status = CheckJni(env, "background LuaApp for surface recreation");
  }
  if (status.ok()) {
    symbols.set_task_scheduler_background_mode(env, native_gl_class, JNI_FALSE,
                                               start_reason);
    status = CheckJni(env, "foreground LuaApp for surface recreation");
  }
  if (status.ok()) {
    symbols.start_app(env, native_gl_class, start_app_params);
    status = CheckJni(env, "restart LuaApp with recreated surface");
  }

  if (start_reason != nullptr)
    env->DeleteLocalRef(start_reason);
  if (stop_reason != nullptr)
    env->DeleteLocalRef(stop_reason);
  return status;
}

}  // namespace runtime
}  // namespace mocktail
