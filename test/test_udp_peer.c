
#ifdef WIN32
    #include "tinylib/windows/net/udp_peer.h"
    #include <winsock2.h>
#elif defined(__linux__)
    #include "tinylib/linux/net/udp_peer.h"
#endif

#include "tinylib/util/log.h"

#include <stdlib.h>
#include <stdio.h>

static loop_t *g_loop;
static udp_peer_t *g_udp_peer;

static 
void on_message(udp_peer_t *udp_peer, void *message, unsigned size, void* userdata, const inetaddr_t *peer_addr)
{
    fwrite(message, 1, size, stdout);
    fwrite("\n", 1, 1, stdout);

    return;
}

int main(int argc, char *argv[])
{
    #ifdef WIN32
    WSADATA wsa_data;
    
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    
    if (argc < 2)
    {
        printf("usage: %s <local port>\n", argv[0]);
        return 0;
    }
    
    log_setlevel(LOG_LEVEL_DEBUG);

    g_loop = loop_new(1);
    g_udp_peer = udp_peer_new(g_loop, "0.0.0.0", (unsigned short)atoi(argv[1]), on_message, NULL, NULL);

    loop_loop(g_loop);

    udp_peer_destroy(g_udp_peer);
    loop_destroy(g_loop);

    #ifdef WIN32
    WSACleanup();
    #endif

    return 0;
}
