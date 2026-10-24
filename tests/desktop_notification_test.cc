#include "runtime/desktop_notification.h"

#include <glib.h>
#include <gtest/gtest.h>

#include <string>

namespace mocktail {
namespace runtime {
namespace {

std::string Print(GVariant* value) {
  gchar* text = g_variant_print(value, TRUE);
  std::string copy = text;
  g_free(text);
  return copy;
}

TEST(DesktopNotificationTest, UsesTheNotificationServiceOutsideFlatpak) {
  DesktopNotificationCall call =
      BuildDesktopNotificationCall(false, "Title", "Body");
  EXPECT_EQ(call.bus_name, "org.freedesktop.Notifications");
  EXPECT_EQ(call.object_path, "/org/freedesktop/Notifications");
  EXPECT_EQ(call.interface_name, "org.freedesktop.Notifications");
  EXPECT_EQ(call.method, "Notify");
  ASSERT_NE(call.parameters, nullptr);
  EXPECT_STREQ(g_variant_get_type_string(call.parameters), "(susssasa{sv}i)");
  const std::string text = Print(call.parameters);
  EXPECT_NE(text.find("'Nightcap'"), std::string::npos) << text;
  EXPECT_NE(text.find("'Title'"), std::string::npos) << text;
  EXPECT_NE(text.find("'Body'"), std::string::npos) << text;
  g_variant_unref(call.parameters);
}

TEST(DesktopNotificationTest, UsesThePortalInsideFlatpak) {
  DesktopNotificationCall call =
      BuildDesktopNotificationCall(true, "Title", "Body");
  EXPECT_EQ(call.bus_name, "org.freedesktop.portal.Desktop");
  EXPECT_EQ(call.object_path, "/org/freedesktop/portal/desktop");
  EXPECT_EQ(call.interface_name, "org.freedesktop.portal.Notification");
  EXPECT_EQ(call.method, "AddNotification");
  ASSERT_NE(call.parameters, nullptr);
  EXPECT_STREQ(g_variant_get_type_string(call.parameters), "(sa{sv})");
  const std::string text = Print(call.parameters);
  EXPECT_NE(text.find("'title': <'Title'>"), std::string::npos) << text;
  EXPECT_NE(text.find("'body': <'Body'>"), std::string::npos) << text;
  g_variant_unref(call.parameters);
}

}
}
}
