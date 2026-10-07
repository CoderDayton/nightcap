#include "runtime/roblox_platform_web_symbols.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "compat/web_view_protocol_contract.h"
#include "linker/linker.h"

namespace mocktail {
namespace runtime {
namespace {

template <typename Function>
Function Resolve(void* library, const char* name) {
  return reinterpret_cast<Function>(
      linker::ResolveSymbol(library, std::string(name)));
}

// Reads the mapped initializer and follows its getter call only when both
// match the code contract for this host's architecture. Both reads stay
// inside libroblox.so's text segment: the initializer is longer than the scan
// window, and the getter address comes from a verified call site.
AcquireWebViewProtocolFn FindWebViewProtocolGetter(const void* initializer) {
  if (initializer == nullptr) return nullptr;
  const auto address = reinterpret_cast<std::uintptr_t>(initializer);
  const std::string_view code(static_cast<const char*>(initializer),
                              compat::kWebViewProtocolInitializerScanBytes);
#if defined(__x86_64__)
  const std::optional<std::uintptr_t> getter =
      compat::FindWebViewProtocolGetterCallX86_64(code, address);
  const bool getter_matches =
      getter.has_value() &&
      compat::HasWebViewProtocolGetterContractX86_64(std::string_view(
          reinterpret_cast<const char*>(*getter),
          compat::kWebViewProtocolGetterBytesX86_64));
#elif defined(__aarch64__)
  const std::optional<std::uintptr_t> getter =
      compat::FindWebViewProtocolGetterCallArm64(code, address);
  const bool getter_matches =
      getter.has_value() &&
      compat::HasWebViewProtocolGetterContractArm64(std::string_view(
          reinterpret_cast<const char*>(*getter),
          compat::kWebViewProtocolGetterBytesArm64));
#else
  const std::optional<std::uintptr_t> getter;
  const bool getter_matches = false;
  (void)code;
  (void)address;
#endif
  return getter_matches ? reinterpret_cast<AcquireWebViewProtocolFn>(*getter)
                        : nullptr;
}

}  // namespace

RobloxPlatformWebSymbols ResolveRobloxPlatformWebSymbols(
    void* roblox_library, SubscribeWebViewRawFn subscribe_raw,
    DeleteWebViewConnectionFn delete_connection) {
  RobloxPlatformWebSymbols symbols;
  if (roblox_library == nullptr) {
    return symbols;
  }

  symbols.web_view.get_open_window_id = Resolve<GetWebViewOpenWindowIdFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getOpenWindowId");
  symbols.web_view.get_handle_window_close_id =
      Resolve<GetWebViewHandleWindowCloseIdFn>(
          roblox_library,
          "Java_com_roblox_protocols_webview_WebViewProtocol_"
          "getHandleWindowCloseId");
  symbols.web_view.get_protocol_name = Resolve<GetWebViewStringFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getProtocolName");
  symbols.web_view.get_is_available_id = Resolve<GetWebViewStringFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getIsAvailableId");
  symbols.web_view.get_message_id = Resolve<GetWebViewMessageIdFn>(
      roblox_library,
      "Java_com_roblox_universalapp_messagebus_MessageBus_getMessageId");
  symbols.web_view.initialize_android_web_view_protocol =
      Resolve<InitializeAndroidWebViewProtocolFn>(
          roblox_library,
          "Java_com_roblox_protocols_webview_WebViewProtocol_"
          "initializeAndroidWebViewProtocol");
  symbols.web_view.acquire_web_view_protocol = FindWebViewProtocolGetter(
      reinterpret_cast<const void*>(
          symbols.web_view.initialize_android_web_view_protocol));
  symbols.web_view.subscribe_raw = subscribe_raw;
  symbols.web_view.delete_connection = delete_connection;
  symbols.web_view.set_request_handler_raw =
      Resolve<SetWebViewRequestHandlerRawFn>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "setRequestHandlerRaw");
  symbols.web_view.clear_request_handler =
      Resolve<ClearWebViewRequestHandlerFn>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "clearRequestHandler");
  symbols.web_view.publish_raw = Resolve<PublishWebViewRawFn>(
      roblox_library,
      "Java_com_roblox_universalapp_messagebus_MessageBus_publishRaw");
  symbols.web_view.broadcast_data_model_focus =
      Resolve<BroadcastWebViewDataModelFocusFn>(
          roblox_library,
          "Java_com_roblox_engine_jni_NativeGLInterface_"
          "nativeBroadcastEventWithNamespace");
  symbols.web_view.get_mutate_window_id = Resolve<GetWebViewMutateWindowIdFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_"
      "getMutateWindowId");
  symbols.web_view.get_close_window_id = Resolve<GetWebViewCloseWindowIdFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getCloseWindowId");
  symbols.web_view.signal_javascript_callback =
      Resolve<SignalWebViewJavascriptCallbackFn>(
          roblox_library,
          "Java_com_roblox_protocols_webview_WebViewProtocol_"
          "signalJavascriptCallback");
  symbols.web_view.update_cookie_set_handler =
      Resolve<UpdateRobloxCookieSetHandlerFn>(
          roblox_library,
          "Java_com_roblox_universalapp_cookie_JNICookieProtocol_"
          "updateOnSetCookieHandler");
  symbols.web_view.send_app_event_on_game_loaded =
      Resolve<SendRobloxAppEventOnGameLoadedFn>(
          roblox_library,
          "Java_com_roblox_engine_jni_NativeGLInterface_"
          "nativeAppBridgeV2SendAppEventOnGameLoaded");
  symbols.web_view.send_app_event_on_app_ready =
      Resolve<SendRobloxAppEventOnAppReadyFn>(
          roblox_library,
          "Java_com_roblox_engine_jni_NativeGLInterface_"
          "nativeAppBridgeV2SendAppEventOnAppReady");

  symbols.browser_service.bind = Resolve<BindRobloxMemStorageFn>(
      roblox_library, "Java_com_roblox_engine_jni_memstorage_MemStorage_bind");
  symbols.browser_service.disconnect = Resolve<DisconnectRobloxMemStorageFn>(
      roblox_library,
      "Java_com_roblox_engine_jni_memstorage_Connection_disconnect");
  symbols.browser_service.release_connection =
      Resolve<ReleaseRobloxMemStorageConnectionFn>(
          roblox_library,
          "Java_com_roblox_engine_jni_memstorage_Connection_"
          "releaseConnection");
  symbols.browser_service.fire = Resolve<FireRobloxMemStorageFn>(
      roblox_library, "Java_com_roblox_engine_jni_memstorage_MemStorage_fire");
  symbols.permissions.subscribe_request =
      Resolve<decltype(symbols.permissions.subscribe_request)>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "doSubscribeProtocolMethodRequestRaw");
  symbols.permissions.delete_connection = delete_connection;
  symbols.permissions.publish_response =
      Resolve<decltype(symbols.permissions.publish_response)>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "publishProtocolMethodResponseRaw");
  symbols.permissions.set_async_handler =
      Resolve<decltype(symbols.permissions.set_async_handler)>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "setRequestHandlerAsyncRaw");
  symbols.permissions.clear_handler = symbols.web_view.clear_request_handler;
  symbols.permissions.call_response_handler =
      Resolve<decltype(symbols.permissions.call_response_handler)>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "callResponseHandlerRaw");
  using AccountStringFn = RobloxAccountProtocolSymbols::GetStringFn;
  constexpr char kAccountPrefix[] =
      "Java_com_roblox_universalapp_account_JNIAccountProtocol_";
  const auto account_getter = [&](const char* name) {
    return Resolve<AccountStringFn>(roblox_library,
                                    (std::string(kAccountPrefix) + name).c_str());
  };
  symbols.account.get_protocol_name = account_getter("getProtocolName");
  symbols.account.get_device_integrity_available_method_name =
      account_getter("getDeviceIntegrityAvailableMethodName");
  symbols.account.get_integrity_token_method_name =
      account_getter("getGetIntegrityTokenMethodName");
  symbols.account.get_support_key = account_getter("getSupportKey");
  symbols.account.get_token_key = account_getter("getTokenKey");
  symbols.account.get_result_key = account_getter("getResultKey");
  symbols.account.set_request_handler_raw =
      symbols.web_view.set_request_handler_raw;
  symbols.account.clear_request_handler = symbols.web_view.clear_request_handler;
  return symbols;
}

}  // namespace runtime
}  // namespace mocktail
