#define _POSIX_C_SOURCE 200112L
#include "tracker_udp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t htonll(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(x & 0xffffffffULL)) << 32) |
           htonl((uint32_t)(x >> 32));
#else
    return x;
#endif
}

static uint64_t ntohll(uint64_t x)
{
    return htonll(x);
}

static int parse_udp_url(const char *url,
                         char *host,
                         size_t host_size,
                         char *port,
                         size_t port_size)
{
    const char *prefix = "udp://";
    size_t prefix_len = strlen(prefix);

    if (strncmp(url, prefix, prefix_len) != 0)
        return 0;

    const char *p = url + prefix_len;
    const char *colon = strchr(p, ':');

    if (!colon)
        return 0;

    size_t host_len = (size_t)(colon - p);

    if (host_len == 0 || host_len >= host_size)
        return 0;

    memcpy(host, p, host_len);
    host[host_len] = '\0';

    const char *port_start = colon + 1;
    const char *slash = strchr(port_start, '/');

    size_t port_len;

    if (slash)
        port_len = (size_t)(slash - port_start);
    else
        port_len = strlen(port_start);

    if (port_len == 0 || port_len >= port_size)
        return 0;

    memcpy(port, port_start, port_len);
    port[port_len] = '\0';

    return 1;
}

static void add_peer(Peer *out_peers,
                     size_t max_peers,
                     size_t *out_peers_count,
                     unsigned int ip1,
                     unsigned int ip2,
                     unsigned int ip3,
                     unsigned int ip4,
                     unsigned int port)
{
    if (ip1 == 127)
        return;

    if (*out_peers_count >= max_peers)
        return;

    char ip_str[16];

    snprintf(ip_str,
             sizeof(ip_str),
             "%u.%u.%u.%u",
             ip1, ip2, ip3, ip4);

    for (size_t i = 0; i < *out_peers_count; i++) {
        if (strcmp(out_peers[i].ip, ip_str) == 0 &&
            out_peers[i].port == (unsigned short)port) {
            printf("  duplicate peer, skipping %s:%u\n", ip_str, port);
            return;
        }
    }

    snprintf(out_peers[*out_peers_count].ip,
             sizeof(out_peers[*out_peers_count].ip),
             "%s",
             ip_str);

    out_peers[*out_peers_count].port = (unsigned short)port;
    (*out_peers_count)++;
}

int tracker_udp_announce(const char *tracker_url,
                         const unsigned char info_hash[20],
                         const unsigned char peer_id[20],
                         long long left,
                         Peer *out_peers,
                         size_t max_peers,
                         size_t *out_peers_count)
{
    char host[256];
    char port[16];

    if (!parse_udp_url(tracker_url, host, sizeof(host), port, sizeof(port))) {
        fprintf(stderr, "bad udp tracker url: %s\n", tracker_url);
        return 0;
    }

    printf("UDP tracker host: %s\n", host);
    printf("UDP tracker port: %s\n", port);

    struct addrinfo hints;
    struct addrinfo *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return 0;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        perror("socket");
        freeaddrinfo(res);
        return 0;
    }

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /*
        STEP 1: UDP connect request
    */

    unsigned char connect_req[16];

    uint64_t protocol_id = htonll(0x41727101980ULL);
    uint32_t action = htonl(0);
    uint32_t transaction_id = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    uint32_t transaction_id_net = htonl(transaction_id);

    memcpy(connect_req + 0, &protocol_id, 8);
    memcpy(connect_req + 8, &action, 4);
    memcpy(connect_req + 12, &transaction_id_net, 4);

    printf("sending UDP connect request\n");

    ssize_t sent = sendto(sock,
                          connect_req,
                          sizeof(connect_req),
                          0,
                          res->ai_addr,
                          res->ai_addrlen);

    if (sent != (ssize_t)sizeof(connect_req)) {
        perror("sendto connect");
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    unsigned char connect_resp[16];

    ssize_t received = recvfrom(sock,
                                connect_resp,
                                sizeof(connect_resp),
                                0,
                                NULL,
                                NULL);

    if (received < 0) {
        perror("recvfrom connect");
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    if (received < 16) {
        fprintf(stderr, "short UDP connect response: %zd\n", received);
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    uint32_t resp_action;
    uint32_t resp_transaction_id;
    uint64_t connection_id;

    memcpy(&resp_action, connect_resp + 0, 4);
    memcpy(&resp_transaction_id, connect_resp + 4, 4);
    memcpy(&connection_id, connect_resp + 8, 8);

    resp_action = ntohl(resp_action);
    resp_transaction_id = ntohl(resp_transaction_id);
    connection_id = ntohll(connection_id);

    if (resp_action != 0) {
        fprintf(stderr, "bad UDP connect action: %u\n", resp_action);
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    if (resp_transaction_id != transaction_id) {
        fprintf(stderr, "bad UDP connect transaction id\n");
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    printf("UDP connection_id: %llu\n",
           (unsigned long long)connection_id);

    /*
        STEP 2: UDP announce request
    */

    unsigned char announce_req[98];

    memset(announce_req, 0, sizeof(announce_req));

    uint64_t connection_id_net = htonll(connection_id);
    uint32_t announce_action = htonl(1);
    uint32_t announce_transaction_id =
        ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    uint32_t announce_transaction_id_net = htonl(announce_transaction_id);

    uint64_t downloaded = htonll(0);
    uint64_t left_net = htonll((uint64_t)left);
    uint64_t uploaded = htonll(0);

    uint32_t event = htonl(2);      /* 2 = started */
    uint32_t ip_address = htonl(0); /* default */
    uint32_t key = htonl((uint32_t)rand());
    uint32_t num_want = htonl(50);
    uint16_t listen_port = htons(6881);

    memcpy(announce_req + 0, &connection_id_net, 8);
    memcpy(announce_req + 8, &announce_action, 4);
    memcpy(announce_req + 12, &announce_transaction_id_net, 4);
    memcpy(announce_req + 16, info_hash, 20);
    memcpy(announce_req + 36, peer_id, 20);
    memcpy(announce_req + 56, &downloaded, 8);
    memcpy(announce_req + 64, &left_net, 8);
    memcpy(announce_req + 72, &uploaded, 8);
    memcpy(announce_req + 80, &event, 4);
    memcpy(announce_req + 84, &ip_address, 4);
    memcpy(announce_req + 88, &key, 4);
    memcpy(announce_req + 92, &num_want, 4);
    memcpy(announce_req + 96, &listen_port, 2);

    printf("sending UDP announce request\n");

    sent = sendto(sock,
                  announce_req,
                  sizeof(announce_req),
                  0,
                  res->ai_addr,
                  res->ai_addrlen);

    if (sent != (ssize_t)sizeof(announce_req)) {
        perror("sendto announce");
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    unsigned char announce_resp[4096];

    received = recvfrom(sock,
                        announce_resp,
                        sizeof(announce_resp),
                        0,
                        NULL,
                        NULL);

    if (received < 0) {
        perror("recvfrom announce");
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    if (received < 20) {
        fprintf(stderr, "short UDP announce response: %zd\n", received);
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    memcpy(&resp_action, announce_resp + 0, 4);
    memcpy(&resp_transaction_id, announce_resp + 4, 4);

    resp_action = ntohl(resp_action);
    resp_transaction_id = ntohl(resp_transaction_id);

    if (resp_action == 3) {
        fprintf(stderr, "UDP tracker error: %.*s\n",
                (int)(received - 8),
                announce_resp + 8);
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    if (resp_action != 1) {
        fprintf(stderr, "bad UDP announce action: %u\n", resp_action);
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    if (resp_transaction_id != announce_transaction_id) {
        fprintf(stderr, "bad UDP announce transaction id\n");
        close(sock);
        freeaddrinfo(res);
        return 0;
    }

    uint32_t interval;
    uint32_t leechers;
    uint32_t seeders;

    memcpy(&interval, announce_resp + 8, 4);
    memcpy(&leechers, announce_resp + 12, 4);
    memcpy(&seeders, announce_resp + 16, 4);

    interval = ntohl(interval);
    leechers = ntohl(leechers);
    seeders = ntohl(seeders);

    printf("UDP tracker interval: %u\n", interval);
    printf("UDP tracker leechers: %u\n", leechers);
    printf("UDP tracker seeders: %u\n", seeders);

    size_t peers_len = (size_t)received - 20;

    if (peers_len % 6 != 0) {
        printf("bad UDP peers length: %zu\n", peers_len);
    }

    size_t peers_count = peers_len / 6;

    printf("UDP peers count: %zu\n", peers_count);

    for (size_t i = 0; i < peers_count; i++) {
        const unsigned char *p = announce_resp + 20 + i * 6;

        unsigned int ip1 = p[0];
        unsigned int ip2 = p[1];
        unsigned int ip3 = p[2];
        unsigned int ip4 = p[3];

        unsigned int peer_port = ((unsigned int)p[4] << 8) | p[5];

        printf("  UDP peer %u.%u.%u.%u:%u\n",
               ip1, ip2, ip3, ip4, peer_port);

        add_peer(out_peers,
                 max_peers,
                 out_peers_count,
                 ip1,
                 ip2,
                 ip3,
                 ip4,
                 peer_port);
    }

    close(sock);
    freeaddrinfo(res);

    return 1;
}
