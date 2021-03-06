
#include "tinylib/linux/net/udp_peer.h"
#include "tinylib/linux/net/socket.h"
#include "tinylib/linux/net/channel.h"
#include "tinylib/linux/net/buffer.h"

#include "tinylib/util/log.h"
#include "tinylib/util/atomic.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

struct udp_peer
{
    atomic_t ref_count;
    
    loop_t *loop;
    char ip[16];
    unsigned short port;
    on_message_f messagecb;
    void *message_userdata;
    
    on_writable_f writecb;
    void *write_userdata;

    int fd;
    channel_t *channel;

    struct{
        unsigned chunk_count;
        unsigned char data[(65535 + 2 + 18) * 8];
    }in_buffer;
};

static 
void udp_peer_onevent(int fd, int event, void* userdata)
{
    udp_peer_t *peer;

    int ret;
    int saved_errno;
    struct sockaddr_in addr;
    socklen_t addr_len;

    unsigned short *chunk_size;
    inetaddr_t *addr_buffer;
    unsigned char *chunk_buffer;

    unsigned chunk_offset;
    unsigned max_chunk_size;

    peer = (udp_peer_t *)userdata;
    
    if ((event & EPOLLOUT) && (NULL != peer->writecb))
    {
        peer->writecb(peer, peer->write_userdata);
    }
    if (event & EPOLLIN)
    {
        peer->in_buffer.chunk_count = 0;
        chunk_offset = 0;
        do
        {
            max_chunk_size = sizeof(peer->in_buffer.data) - (chunk_offset + sizeof(*chunk_size) + sizeof(*addr_buffer));
            if (max_chunk_size < 65507) /* 65507 = 65535 - 20 - 8 */
            {
                /* 保险起见，当剩下的数据空间，不足65535时，暂停收包，将已收的消息数据交给应用来处理，腾出空间供下次收包
                 * 防备下一个消息达65535时，盲目收包造成消息截断，数据丢失
                 */
                break;
            }

            chunk_size = (unsigned short *)(peer->in_buffer.data + chunk_offset);
            addr_buffer = (inetaddr_t *)(peer->in_buffer.data + chunk_offset + sizeof(*chunk_size));
            chunk_buffer = peer->in_buffer.data + chunk_offset + sizeof(*chunk_size) + sizeof(*addr_buffer);

            memset(&addr, 0, sizeof(addr));
            addr_len = sizeof(addr);
            ret = recvfrom(peer->fd, chunk_buffer, max_chunk_size, 0, (struct sockaddr*)&addr, &addr_len);
            if (ret >= 0)
            {
                *chunk_size = (unsigned short)ret;
                inetaddr_init(addr_buffer, &addr);
                
                chunk_offset += sizeof(*chunk_size) + sizeof(*addr_buffer) + ret;
                peer->in_buffer.chunk_count++;
            }
            else
            {
                saved_errno = errno;
                if (ECONNRESET != saved_errno && EAGAIN != saved_errno)
                {
                    log_error("udp_peer_onevent: recvfrom() failed, errno: %d, peer: %s:%u", saved_errno, peer->ip, peer->port);
                }

                break;
            }
        } while (1);
        
        if (peer->in_buffer.chunk_count > 0)
        {
            if (NULL != peer->messagecb)
            {
                chunk_offset = 0;
                do
                {
                    chunk_size = (unsigned short *)(peer->in_buffer.data + chunk_offset);
                    addr_buffer = (inetaddr_t *)(peer->in_buffer.data + chunk_offset + sizeof(*chunk_size));
                    chunk_buffer = peer->in_buffer.data + chunk_offset + sizeof(*chunk_size) + sizeof(*addr_buffer);
                    peer->messagecb(peer, chunk_buffer, *chunk_size, peer->message_userdata, addr_buffer);
                    
                    chunk_offset += sizeof(*chunk_size) + sizeof(*addr_buffer) + *chunk_size;
                    peer->in_buffer.chunk_count--;
                } while(peer->in_buffer.chunk_count > 0);
            }
            else
            {
                log_warn("udp_peer(%s:%u): no message callback was found, all received data will be dropped", peer->ip, peer->port);
            }
        }
    }

    return;
}

static
void init_udp_peer_event(void *userdata)
{
    udp_peer_t* peer = (udp_peer_t*)userdata;
    int event;
    
    event = EPOLLIN;
    if (NULL != peer->writecb)
    {
        event |= EPOLLOUT;
    }
    channel_setevent(peer->channel, event);

    return;
}

udp_peer_t* udp_peer_new(loop_t *loop, const char *ip, unsigned short port, on_message_f messagecb, on_writable_f writecb, void *userdata)
{
    int fd;
    udp_peer_t* peer;

    if (NULL == loop || NULL == ip || 0 == port || NULL == messagecb)
    {
        log_error("udp_peer_new: bad loop(%p) or bad ip(%p) or bad port(%p) or bad messagecb(%p)", loop, ip, port, messagecb);
        return NULL;
    }

    fd = create_udp_socket(port, ip);
    if (fd < 0)
    {
        log_error("udp_peer_new: create_udp_socket() failed, local addr: %s:%u, errno: %d", ip, port, errno);
        return NULL;
    }

    peer = (udp_peer_t*)malloc(sizeof(udp_peer_t));
    memset(peer, 0, sizeof(*peer));

    peer->ref_count = 1;
    peer->loop = loop;
    strncpy(peer->ip, ip, 16);
    peer->port = port;
    peer->messagecb = messagecb;
    peer->message_userdata = userdata;
    peer->writecb = writecb;
    peer->write_userdata = userdata;

    peer->fd = fd;
    peer->channel = channel_new(fd, peer->loop, udp_peer_onevent, peer);

    loop_run_inloop(peer->loop, init_udp_peer_event, peer);

    return peer;
}

struct udp_peer_notify
{
    udp_peer_t* peer;

    on_message_f messagecb;
    void *message_userdata;
    on_writable_f writecb;
    void *write_userdata;
};

static inline
void delete_udp_peer(udp_peer_t* peer)
{
    channel_detach(peer->channel);
    channel_destroy(peer->channel);
    close(peer->fd);
    free(peer);
    
    return;
}

static 
void do_udp_peer_onmessage(void *userdata)
{
    struct udp_peer_notify *notify;
    udp_peer_t* peer;
    
    notify = (struct udp_peer_notify *)userdata;
    peer = notify->peer;
    
    peer->messagecb = notify->messagecb;
    peer->message_userdata = notify->message_userdata;
    free(notify);
    
    if (atomic_dec(&peer->ref_count) > 1)
    {
        if (NULL == peer->messagecb)
        {
            channel_clearevent(peer->channel, EPOLLIN);
            peer->message_userdata = NULL;
        }
        else
        {
            channel_setevent(peer->channel, EPOLLIN);
        }
    }
    else
    {
        delete_udp_peer(peer);
    }

    return;
}

on_message_f udp_peer_onmessage(udp_peer_t* peer, on_message_f messagecb, void *userdata)
{
    on_message_f old_messagecb;
    struct udp_peer_notify *notify;
    
    if (NULL == peer)
    {
        log_error("udp_peer_onmessage: bad peer");
        return NULL;
    }

    old_messagecb = peer->messagecb;

    notify = (struct udp_peer_notify *)malloc(sizeof(*notify));    
    memset(notify, 0, sizeof(*notify));

    (void)atomic_inc(&peer->ref_count);
    notify->peer = peer;
    notify->messagecb = messagecb;
    notify->message_userdata = userdata;
    loop_run_inloop(peer->loop, do_udp_peer_onmessage, notify);

    return old_messagecb;
}

static 
void do_udp_peer_onwrite(void *userdata)
{
    struct udp_peer_notify *notify;
    udp_peer_t* peer;
    
    notify = (struct udp_peer_notify *)userdata;
    peer = notify->peer;
    
    peer->writecb = notify->writecb;
    peer->write_userdata = notify->write_userdata;
    free(notify);
    
    if (atomic_dec(&peer->ref_count) > 1)
    {
        if (NULL == peer->writecb)
        {
            channel_clearevent(peer->channel, EPOLLOUT);
            peer->message_userdata = NULL;
        }
        else
        {
            channel_setevent(peer->channel, EPOLLOUT);
        }    
    }
    else
    {
        delete_udp_peer(peer);
    }

    return;
}

on_writable_f udp_peer_onwrite(udp_peer_t* peer, on_writable_f writecb, void *userdata)
{
    on_writable_f old_writecb;
    struct udp_peer_notify *notify;

    if (NULL == peer)
    {
        log_error("udp_peer_onwrite: bad peer");
        return NULL;
    }

    old_writecb = peer->writecb;

    notify = (struct udp_peer_notify *)malloc(sizeof(*notify));    
    memset(notify, 0, sizeof(*notify));

    (void)atomic_inc(&peer->ref_count);
    notify->peer = peer;
    notify->writecb = writecb;
    notify->write_userdata = userdata;
    loop_run_inloop(peer->loop, do_udp_peer_onwrite, notify);

    return old_writecb;
}

unsigned short udp_peer_getport(udp_peer_t* peer)
{
    if (NULL == peer)
    {
        return 0;
    }
    
    return peer->port;
}

static 
void do_udp_peer_destroy(void *userdata)
{
    udp_peer_t* peer = (udp_peer_t*)userdata;

    if (atomic_dec(&peer->ref_count) == 1)
    {
        delete_udp_peer(peer);
    }

    return;
}

void udp_peer_destroy(udp_peer_t* peer)
{
    if (NULL == peer)
    {
        return;
    }

    loop_run_inloop(peer->loop, do_udp_peer_destroy, peer);

    return;
}

/* 由于udp的简单性，请使用者自行完成报文分片，保证每次的message尺寸小于mtu, 本发送接口只做简单发送，不做缓存重发 */
int udp_peer_send(udp_peer_t* peer, const void *message, unsigned len, const inetaddr_t *peer_addr)
{
    struct sockaddr_in addr;
    int ret;
    int result;

    if (NULL == peer || NULL == message || 0 == len || 65535 < len || NULL == peer_addr)
    {
        log_error("udp_peer_send: bad peer(%p) or bad message(%p) or bad len(%u) or bad peer_addr(%p)", 
            peer, message, len, peer_addr);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(peer_addr->ip);
    addr.sin_port = htons(peer_addr->port);

    ret = 0;
    result = sendto(peer->fd, message, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    if (len != result)
    {
        ret = -1;
    }

    return ret;
}

int udp_peer_send2(udp_peer_t* peer, const void *message, unsigned len, const struct sockaddr_in *peer_addr)
{
    int ret;
    int result;

    if (NULL == peer || NULL == message || 0 == len || 65535 < len  || NULL == peer_addr)
    {
        log_error("udp_peer_send2: bad peer(%p) or bad message(%p) or bad len(%u) or bad peer_addr(%p)", 
            peer, message, len, peer_addr);
        return -1;
    }

    ret = 0;
    result = sendto(peer->fd, message, len, 0, (const struct sockaddr*)peer_addr, sizeof(*peer_addr));
    if (len != result)
    {
        log_warn("udp_peer_send2: sendto() failed, errno: %d", errno);
        ret = -1;
    }

    return ret;
}

void udp_peer_expand_send_buffer(udp_peer_t* peer, unsigned size)
{
    int result;
    int send_buffer_size;
    socklen_t len;
    
    if (NULL == peer)
    {
        return;
    }

    len = sizeof(send_buffer_size);
    result = getsockopt(peer->fd, SOL_SOCKET, SO_SNDBUF, (char*)&send_buffer_size, &len);
    if (0 != result)
    {
        log_error("udp_peer_expand_send_buffer:　getsockopt() failed, errno: %d", errno);
        return;
    }
    
    send_buffer_size += ((size+1023)>>10)<<10;
    setsockopt(peer->fd, SOL_SOCKET, SO_SNDBUF, (char*)&send_buffer_size, sizeof(send_buffer_size));
    
    return;
}

void udp_peer_expand_recv_buffer(udp_peer_t* peer, unsigned size)
{
    int result;
    int recv_buffer_size;
    socklen_t len;
    
    if (NULL == peer)
    {
        return;
    }

    len = sizeof(recv_buffer_size);
    result = getsockopt(peer->fd, SOL_SOCKET, SO_RCVBUF, (char*)&recv_buffer_size, &len);
    if (0 != result)
    {
        log_error("udp_peer_expand_recv_buffer:　getsockopt() failed, errno: %d", errno);
        return;
    }
    
    recv_buffer_size += ((size+1023)>>10)<<10;;
    setsockopt(peer->fd, SOL_SOCKET, SO_RCVBUF, (char*)&recv_buffer_size, sizeof(recv_buffer_size));
    
    return;
}
