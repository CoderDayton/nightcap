#ifndef MOCKTAIL_RUNTIME_ROBLOX_LUA_APP_RESTART_H_
#define MOCKTAIL_RUNTIME_ROBLOX_LUA_APP_RESTART_H_

#include <jni.h>

#include "mocktail/status.h"
#include "runtime/roblox_app_lifecycle.h"
#include "runtime/roblox_capability_resolver.h"

namespace mocktail {
namespace runtime {

struct LuaAppRestartSymbols {
  NativeAppBridgeV2PauseAppFn pause_app = nullptr;
  SetTaskSchedulerBackgroundModeFn set_task_scheduler_background_mode = nullptr;
  NativeAppBridgeV2StartAppWithParamsFn start_app = nullptr;
};

// Restarts the LuaApp onto a recreated surface: pause, cycle the task
// scheduler through "ASMA.stop" and "ASMA.start", then start it with
// `start_app_params`. Stops at the first step that raises a JNI exception.
Status RestartLuaApp(JNIEnv* env, jclass native_gl_class,
                     const LuaAppRestartSymbols& symbols,
                     jobject start_app_params);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_LUA_APP_RESTART_H_
