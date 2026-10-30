/* UDP transport glue for the SIP UA: the only file touching sockets.
 * Plain BSD sockets, so it builds against lwIP (ESP-IDF) and POSIX (host). */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include "sip_msg.h"
#include "sip_ua.h"

static void to_sockaddr(const sip_addr_t *a, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(a->port);
    sa->sin_addr.s_addr = htonl(a->ip);
}

sip_err_t sip_udp_open(sip_udp_t *u, uint16_t port)
{
    u->fd = -1;
    u->port = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return SIP_ERR_TRANSPORT;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return SIP_ERR_TRANSPORT;
    }
    socklen_t sl = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
        close(fd);
        return SIP_ERR_TRANSPORT;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        close(fd);
        return SIP_ERR_TRANSPORT;
    }
    u->fd = fd;
    u->port = ntohs(sa.sin_port);
    return SIP_OK;
}

void sip_udp_close(sip_udp_t *u)
{
    if (u->fd >= 0) {
        close(u->fd);
    }
    u->fd = -1;
}

int sip_udp_send(void *ctx, const sip_addr_t *to, const void *data, size_t len)
{
    sip_udp_t *u = ctx;
    struct sockaddr_in sa;
    to_sockaddr(to, &sa);
    ssize_t n = sendto(u->fd, data, len, 0, (struct sockaddr *)&sa, sizeof(sa));
    return n == (ssize_t)len ? 0 : -1;
}

int sip_udp_poll(sip_udp_t *u, sip_ua_t *ua, int timeout_ms)
{
    return sip_udp_poll_wake(u, ua, timeout_ms, -1);
}

int sip_udp_poll_wake(sip_udp_t *u, sip_ua_t *ua, int timeout_ms, int wake_fd)
{
    fd_set rf;
    FD_ZERO(&rf);
    int maxfd = -1;
    if (u->fd >= 0) {
        FD_SET(u->fd, &rf);
        maxfd = u->fd;
    }
    if (wake_fd >= 0) {
        FD_SET(wake_fd, &rf);
        if (wake_fd > maxfd) {
            maxfd = wake_fd;
        }
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int r = select(maxfd + 1, &rf, NULL, NULL, &tv);
    if (r < 0) {
        return errno == EINTR ? 0 : SIP_ERR_TRANSPORT;
    }
    if (r == 0) {
        return 0;
    }
    if (wake_fd >= 0 && FD_ISSET(wake_fd, &rf)) {
        uint64_t v;
        (void)!read(wake_fd, &v, sizeof(v));  /* eventfd: resets the counter */
    }
    if (u->fd < 0 || !FD_ISSET(u->fd, &rf) || ua == NULL) {
        return 0;
    }
    /* one byte more than the UA accepts so oversized datagrams are detected */
    static char buf[SIP_MAX_MSG_IN + 1];
    int fed = 0;
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        ssize_t n = recvfrom(u->fd, buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
        if (n < 0) {
            break; /* EWOULDBLOCK: drained */
        }
        if (sa.sin_family != AF_INET) {
            continue;
        }
        sip_addr_t from = { .ip = ntohl(sa.sin_addr.s_addr), .port = ntohs(sa.sin_port) };
        sip_ua_on_packet(ua, buf, (size_t)n, &from);
        fed++;
    }
    return fed;
}

sip_err_t sip_udp_resolve(const char *host, uint16_t port, sip_addr_t *out)
{
    uint32_t ip;
    if (sip_ip_parse(host, &ip)) {
        out->ip = ip;
        out->port = port;
        return SIP_OK;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return SIP_ERR_TRANSPORT;
    }
    const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
    out->ip = ntohl(sa->sin_addr.s_addr);
    out->port = port;
    freeaddrinfo(res);
    return SIP_OK;
}

sip_err_t sip_udp_local_ip_for(const sip_addr_t *dest, uint32_t *ip)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return SIP_ERR_TRANSPORT;
    }
    struct sockaddr_in sa;
    to_sockaddr(dest, &sa);
    sip_err_t err = SIP_ERR_TRANSPORT;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        socklen_t sl = sizeof(sa);
        if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0) {
            *ip = ntohl(sa.sin_addr.s_addr);
            err = SIP_OK;
        }
    }
    close(fd);
    return err;
}
