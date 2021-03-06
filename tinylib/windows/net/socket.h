
#ifndef TINYLIB_NET_SOCKET_H
#define TINYLIB_NET_SOCKET_H

#include <winsock2.h>

#ifdef __cplusplus
extern "C" {
#endif

SOCKET create_server_socket(unsigned short port, const char* ip);

SOCKET create_client_socket(void);

void set_socket_reuseaddr(SOCKET fd, int on);

void set_socket_onblock(SOCKET fd, int on);

void set_socket_nodelay(SOCKET fd, int on);

SOCKET create_udp_socket(unsigned short port, const char *ip);

/* type: SOCK_STREAM,SOCK_DGRAM */
int socketpair(int type, SOCKET fds[2]);

#ifdef __cplusplus
}
#endif

#endif /* !TINYLIB_NET_SOCKET_H */
