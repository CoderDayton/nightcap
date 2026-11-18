#ifndef MOCKTAIL_PLATFORM_LOG_REDACTION_H_
#define MOCKTAIL_PLATFORM_LOG_REDACTION_H_

#include <string>
#include <string_view>

namespace mocktail::platform {

std::string RedactJoinTickets(std::string_view text);

}

#endif
