#include "runtime/desktop_notification.h"

#include <gio/gio.h>

#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

namespace mocktail {
namespace runtime {

namespace {

constexpr int kNotificationCallTimeoutMs = 2000;

}

DesktopNotificationCall BuildDesktopNotificationCall(bool sandboxed,
                                                     std::string_view title,
                                                     std::string_view body) {
  const std::string owned_title(title);
  const std::string owned_body(body);
  DesktopNotificationCall call;
  if (sandboxed) {
    call.bus_name = "org.freedesktop.portal.Desktop";
    call.object_path = "/org/freedesktop/portal/desktop";
    call.interface_name = "org.freedesktop.portal.Notification";
    call.method = "AddNotification";
    GVariantBuilder fields;
    g_variant_builder_init(&fields, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&fields, "{sv}", "title",
                          g_variant_new_string(owned_title.c_str()));
    g_variant_builder_add(&fields, "{sv}", "body",
                          g_variant_new_string(owned_body.c_str()));
    call.parameters = g_variant_ref_sink(
        g_variant_new("(sa{sv})", "nightcap", &fields));
    return call;
  }
  call.bus_name = "org.freedesktop.Notifications";
  call.object_path = "/org/freedesktop/Notifications";
  call.interface_name = "org.freedesktop.Notifications";
  call.method = "Notify";
  call.parameters = g_variant_ref_sink(g_variant_new(
      "(susssasa{sv}i)", "Nightcap", 0u, "io.github.CoderDayton.nightcap",
      owned_title.c_str(), owned_body.c_str(), nullptr, nullptr, -1));
  return call;
}

void ShowDesktopNotification(std::string title, std::string body) {
  std::thread([title = std::move(title), body = std::move(body)] {
    DesktopNotificationCall call = BuildDesktopNotificationCall(
        std::getenv("FLATPAK_ID") != nullptr, title, body);
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (bus != nullptr) {
      GVariant* reply = g_dbus_connection_call_sync(
          bus, call.bus_name.c_str(), call.object_path.c_str(),
          call.interface_name.c_str(), call.method.c_str(), call.parameters,
          nullptr, G_DBUS_CALL_FLAGS_NONE, kNotificationCallTimeoutMs, nullptr,
          &error);
      if (reply != nullptr) {
        g_variant_unref(reply);
      }
      g_object_unref(bus);
    }
    if (error != nullptr) {
      std::cerr << "  [notify] desktop notification failed: " << error->message
                << '\n';
      g_clear_error(&error);
    }
    g_variant_unref(call.parameters);
  }).detach();
}

void NotifyWebRobuxPurchase(bool browser_opened) {
  if (browser_opened) {
    ShowDesktopNotification(
        "Robux purchase opened in your browser",
        "Buy Robux there, then try the purchase in Roblox again.");
    return;
  }
  ShowDesktopNotification("Couldn't open your browser",
                          "Buy Robux at roblox.com/upgrades/robux.");
}

}
}
