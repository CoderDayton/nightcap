#include "mocktail/platform/log_redaction.h"

#include <cstddef>

namespace mocktail::platform {
namespace {

constexpr std::string_view kTicket = "ticket=";
constexpr std::string_view kRedacted = "[redacted]";

char Lower(char byte) {
  return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a')
                                    : byte;
}

bool Alphanumeric(char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

bool EndsToken(char byte) {
  const auto value = static_cast<unsigned char>(byte);
  return value <= 0x20 || value == 0x7f;
}

bool StartsTicket(std::string_view text, std::size_t at) {
  if (text.size() - at < kTicket.size() ||
      (at > 0 && Alphanumeric(text[at - 1]))) {
    return false;
  }
  for (std::size_t index = 0; index < kTicket.size(); ++index) {
    if (Lower(text[at + index]) != kTicket[index]) {
      return false;
    }
  }
  return true;
}

std::size_t TicketEnd(std::string_view text, std::size_t begin) {
  bool in_json = false;
  for (std::size_t at = begin; at < text.size() && !EndsToken(text[at]);
       ++at) {
    if (text[at] == '"') {
      in_json = at > begin && text[at - 1] == '\\';
      break;
    }
  }
  std::size_t end = begin;
  while (end < text.size() && !EndsToken(text[end]) &&
         !(in_json && text[end] == '"' && text[end - 1] != '\\')) {
    ++end;
  }
  return end;
}

}

std::string RedactJoinTickets(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    if (!StartsTicket(text, at)) {
      out.push_back(text[at++]);
      continue;
    }
    const std::size_t value = at + kTicket.size();
    const std::size_t end = TicketEnd(text, value);
    out.append(text.substr(at, kTicket.size()));
    if (end > value) {
      out.append(kRedacted);
    }
    at = end;
  }
  return out;
}

}
