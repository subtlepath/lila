#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#define lwip_socket ::socket
#define lwip_fcntl ::fcntl
#define lwip_bind ::bind
#define lwip_listen ::listen
#define lwip_accept ::accept
#define lwip_recv ::recv
#define lwip_close ::close
inline ssize_t lwip_send(int fd, const void* bytes, size_t length, int flags) {
  return ::send(fd, bytes, length, flags | MSG_NOSIGNAL);
}
