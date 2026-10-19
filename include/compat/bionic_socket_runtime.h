#ifndef MOCKTAIL_COMPAT_BIONIC_SOCKET_RUNTIME_H_
#define MOCKTAIL_COMPAT_BIONIC_SOCKET_RUNTIME_H_

#include <sys/socket.h>

extern "C" {

int mocktail_bionic_setsockopt(int socket, int level, int option_name,
                               const void *option_value,
                               socklen_t option_length);

ssize_t mocktail_bionic_sendmsg(int socket, const struct msghdr *message,
                                int flags);

using MocktailDatagramObserver = void (*)(bool outgoing,
                                          const struct sockaddr *peer,
                                          socklen_t peer_length,
                                          ssize_t bytes);

void mocktail_bionic_set_datagram_observer(MocktailDatagramObserver observer);

ssize_t mocktail_bionic_sendto(int socket, const void *buffer, size_t length,
                               int flags, const struct sockaddr *destination,
                               socklen_t destination_length);

ssize_t mocktail_bionic_sendto_chk(int socket, const void *buffer,
                                   size_t length, size_t buffer_length,
                                   int flags,
                                   const struct sockaddr *destination,
                                   socklen_t destination_length);

ssize_t mocktail_bionic_recvfrom(int socket, void *buffer, size_t length,
                                 int flags, struct sockaddr *source,
                                 socklen_t *source_length);

}  // extern "C"

#endif  // MOCKTAIL_COMPAT_BIONIC_SOCKET_RUNTIME_H_
