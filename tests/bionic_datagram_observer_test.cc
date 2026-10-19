#include "compat/bionic_socket_runtime.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <vector>

namespace {

struct Observed {
  bool outgoing = false;
  std::uint16_t peer_port = 0;
  ssize_t bytes = 0;
};

std::vector<Observed>* g_observed = nullptr;

void Record(bool outgoing, const sockaddr* peer, socklen_t peer_length,
            ssize_t bytes) {
  Observed observed;
  observed.outgoing = outgoing;
  observed.bytes = bytes;
  if (peer != nullptr && peer_length >= sizeof(sockaddr_in) &&
      peer->sa_family == AF_INET) {
    observed.peer_port =
        ntohs(reinterpret_cast<const sockaddr_in*>(peer)->sin_port);
  }
  g_observed->push_back(observed);
}

int BoundUdpSocket(sockaddr_in* address) {
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  address->sin_family = AF_INET;
  address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address->sin_port = 0;
  bind(socket_fd, reinterpret_cast<sockaddr*>(address), sizeof(*address));
  socklen_t length = sizeof(*address);
  getsockname(socket_fd, reinterpret_cast<sockaddr*>(address), &length);
  return socket_fd;
}

TEST(BionicDatagramObserverTest, ReportsEachDatagramWithItsPeer) {
  std::vector<Observed> observed;
  g_observed = &observed;
  sockaddr_in client_address{};
  sockaddr_in server_address{};
  const int client = BoundUdpSocket(&client_address);
  const int server = BoundUdpSocket(&server_address);
  ASSERT_GE(client, 0);
  ASSERT_GE(server, 0);

  mocktail_bionic_set_datagram_observer(&Record);
  const char payload[54] = {};
  ASSERT_EQ(mocktail_bionic_sendto(
                client, payload, sizeof(payload), 0,
                reinterpret_cast<const sockaddr*>(&server_address),
                sizeof(server_address)),
            54);
  char buffer[128];
  sockaddr_in from{};
  socklen_t from_length = sizeof(from);
  ASSERT_EQ(recvfrom(server, buffer, sizeof(buffer), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_length),
            54);
  ASSERT_EQ(sendto(server, payload, 20, 0, reinterpret_cast<sockaddr*>(&from),
                   from_length),
            20);
  from_length = sizeof(from);
  ASSERT_EQ(mocktail_bionic_recvfrom(client, buffer, sizeof(buffer), 0,
                                     reinterpret_cast<sockaddr*>(&from),
                                     &from_length),
            20);
  mocktail_bionic_set_datagram_observer(nullptr);
  ASSERT_EQ(mocktail_bionic_sendto(
                client, payload, 8, 0,
                reinterpret_cast<const sockaddr*>(&server_address),
                sizeof(server_address)),
            8);

  ASSERT_EQ(observed.size(), 2U);
  EXPECT_TRUE(observed[0].outgoing);
  EXPECT_EQ(observed[0].peer_port, ntohs(server_address.sin_port));
  EXPECT_EQ(observed[0].bytes, 54);
  EXPECT_FALSE(observed[1].outgoing);
  EXPECT_EQ(observed[1].peer_port, ntohs(server_address.sin_port));
  EXPECT_EQ(observed[1].bytes, 20);
  close(client);
  close(server);
  g_observed = nullptr;
}

TEST(BionicDatagramObserverTest, FortifiedSendtoIsObserved) {
  std::vector<Observed> observed;
  g_observed = &observed;
  sockaddr_in client_address{};
  sockaddr_in server_address{};
  const int client = BoundUdpSocket(&client_address);
  const int server = BoundUdpSocket(&server_address);
  ASSERT_GE(client, 0);
  ASSERT_GE(server, 0);

  mocktail_bionic_set_datagram_observer(&Record);
  const char payload[54] = {};
  ASSERT_EQ(mocktail_bionic_sendto_chk(
                client, payload, sizeof(payload), sizeof(payload), 0,
                reinterpret_cast<const sockaddr*>(&server_address),
                sizeof(server_address)),
            54);
  mocktail_bionic_set_datagram_observer(nullptr);

  ASSERT_EQ(observed.size(), 1U);
  EXPECT_TRUE(observed[0].outgoing);
  EXPECT_EQ(observed[0].peer_port, ntohs(server_address.sin_port));
  EXPECT_EQ(observed[0].bytes, 54);
  close(client);
  close(server);
  g_observed = nullptr;
}

TEST(BionicDatagramObserverTest, FailedCallsReportNothingAndKeepErrno) {
  std::vector<Observed> observed;
  g_observed = &observed;
  mocktail_bionic_set_datagram_observer(&Record);
  char buffer[8];
  errno = 0;
  EXPECT_EQ(mocktail_bionic_recvfrom(-1, buffer, sizeof(buffer), 0, nullptr,
                                     nullptr),
            -1);
  EXPECT_EQ(errno, EBADF);
  mocktail_bionic_set_datagram_observer(nullptr);
  EXPECT_TRUE(observed.empty());
  g_observed = nullptr;
}

}
