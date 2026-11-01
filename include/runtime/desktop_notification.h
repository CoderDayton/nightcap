#ifndef MOCKTAIL_RUNTIME_DESKTOP_NOTIFICATION_H_
#define MOCKTAIL_RUNTIME_DESKTOP_NOTIFICATION_H_

#include <string>
#include <string_view>

typedef struct _GVariant GVariant;

namespace mocktail {
namespace runtime {

struct DesktopNotificationCall {
  std::string bus_name;
  std::string object_path;
  std::string interface_name;
  std::string method;
  GVariant* parameters = nullptr;
};

DesktopNotificationCall BuildDesktopNotificationCall(bool sandboxed,
                                                     std::string_view title,
                                                     std::string_view body);
void ShowDesktopNotification(std::string title, std::string body);
void NotifyWebRobuxPurchase(bool browser_opened);

}
}

#endif
