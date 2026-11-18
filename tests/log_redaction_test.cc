#include "mocktail/platform/log_redaction.h"

#include <gtest/gtest.h>

namespace mocktail::platform {
namespace {

TEST(LogRedactionTest, RedactsAPlainJoinTicketToTheEndOfTheUrl) {
  EXPECT_EQ(
      RedactJoinTickets(
          "doTeleport: joinScriptUrl https://assetgame.roblox.com/Game/"
          "Join.ashx?ticketVersion=2&ticket={\"UserId\"%3a1%2c\"UserName\"%3a"
          "\"me\"%2c\"Sig\"%3a\"a+b\"}"),
      "doTeleport: joinScriptUrl https://assetgame.roblox.com/Game/"
      "Join.ashx?ticketVersion=2&ticket=[redacted]");
  EXPECT_EQ(RedactJoinTickets("Join.ashx?ticket=abc def"),
            "Join.ashx?ticket=[redacted] def");
}

TEST(LogRedactionTest, RedactsAJoinTicketToTheEndOfItsJsonString) {
  EXPECT_EQ(
      RedactJoinTickets(
          R"({"joinScriptUrl":"https://a/Join.ashx?ticketVersion=2&ticket={\"UserId\"%3a1%2c\"UserName\"%3a\"me\"}&x=1","next":1})"),
      R"({"joinScriptUrl":"https://a/Join.ashx?ticketVersion=2&ticket=[redacted]","next":1})");
}

TEST(LogRedactionTest, LeavesOtherTextAlone) {
  EXPECT_EQ(RedactJoinTickets("ticketVersion=2 noticket=1 ticket="),
            "ticketVersion=2 noticket=1 ticket=");
  EXPECT_EQ(RedactJoinTickets("no tickets here"), "no tickets here");
  EXPECT_EQ(RedactJoinTickets(""), "");
}

TEST(LogRedactionTest, RedactsEveryTicketInALine) {
  EXPECT_EQ(RedactJoinTickets("a ticket=1 b TICKET=2"),
            "a ticket=[redacted] b TICKET=[redacted]");
}

}
}
