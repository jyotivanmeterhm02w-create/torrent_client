#include "peer.h"
#include <stdint.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <errno.h>

static int send_all(int sock, const unsigned char *buf, size_t len);

int peer_send_interested(int sock)
{
    unsigned char msg[5];

    msg[0] = 0;
    msg[1] = 0;
    msg[2] = 0;
    msg[3] = 1;
    msg[4] = 2;

    return send_all(sock, msg, 5);
}

int peer_set_timeout(int sock, int seconds)
{
    struct timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_SNDTIMEO");
        return -1;
    }

    return 0;
}





static int send_all(int sock, const unsigned char *buf, size_t len)
{
    size_t sent_total = 0;

    while (sent_total < len) {
        ssize_t sent = send(sock,
                            buf + sent_total,
                            len - sent_total,
                            0);

        if (sent < 0) {
            perror("send");
            return 0;
        }

        if (sent == 0) {
            fprintf(stderr, "send returned 0\n");
            return 0;
        }

        sent_total += (size_t)sent;
    }

    return 1;
}

static int recv_all(int sock, unsigned char *buf, size_t len)
{
    size_t received = 0;

    while (received < len) {
        ssize_t n = recv(sock, buf + received, len - received, 0);

        if (n == 0) {
            fprintf(stderr, "peer closed connection\n");
            return -1;
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "recv timeout\n");
            } else {
                perror("recv");
            }

            return -1;
        }

        received += n;
    }

    return 0;
}
int peer_read_message(int sock,
                      unsigned char *msg_id,
                      unsigned char *payload,
		      uint32_t payload_capacity,
                      uint32_t *payload_len)
{
    unsigned char len_buf[4];

    if (recv_all(sock, len_buf, 4) < 0)
        return -1;

    uint32_t len =
        ((uint32_t)len_buf[0] << 24) |
        ((uint32_t)len_buf[1] << 16) |
        ((uint32_t)len_buf[2] << 8) |
        ((uint32_t)len_buf[3]);

    if (len == 0) {
        *msg_id = 255;      // keep-alive
        *payload_len = 0;
        return 0;
    }

    if (recv_all(sock, msg_id, 1) < 0)
        return -1;

    *payload_len = len - 1;

    if (*payload_len > 0) {
        if (recv_all(sock, payload, *payload_len) < 0)
            return -1;
    }

    return 0;
}

static void write_u32_be(unsigned char *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

int peer_send_request(int sock,
                      uint32_t piece_index,
                      uint32_t begin,
                      uint32_t length)
{
    unsigned char msg[17];

    /*
        length prefix = 13
        id = 6
        payload:
            index  4 bytes
            begin  4 bytes
            length 4 bytes
    */

    write_u32_be(msg + 0, 13);
    msg[4] = 6;

    write_u32_be(msg + 5, piece_index);
    write_u32_be(msg + 9, begin);
    write_u32_be(msg + 13, length);

    return send_all(sock, msg, sizeof(msg));
}

int peer_connect_handshake(const char *ip,
                   unsigned short port,
                   const unsigned char info_hash[20],
                   const unsigned char peer_id[20])
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        return -1;
    }

    peer_set_timeout(sock,3);

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad peer ip: %s\n", ip);
        close(sock);
        return -1;
    }

    printf("connecting to peer %s:%u\n", ip, port);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    unsigned char handshake[68];

    memset(handshake, 0, sizeof(handshake));

    handshake[0] = 19;
    memcpy(handshake + 1, "BitTorrent protocol", 19);


    memcpy(handshake + 28, info_hash, 20);
    memcpy(handshake + 48, peer_id, 20);

    printf("sending handshake\n");

    if (send_all(sock, handshake, sizeof(handshake)) < 0) {
        fprintf(stderr, "failed to send handshake\n");
        close(sock);
        return -1;
    }

    unsigned char response[68];

    printf("waiting for handshake response\n");

    if (recv_all(sock, response, sizeof(response)) < 0) {
        fprintf(stderr, "failed to receive handshake response\n");
        close(sock);
        return -1;
    }

    if (response[0] != 19 ||
        memcmp(response + 1, "BitTorrent protocol", 19) != 0) {
        fprintf(stderr, "bad handshake protocol\n");

	for (int i = 0; i < 20; i++) {
		fprintf(stderr, "%02x ", response[i]);
	}

	fprintf(stderr, "\n");

	close(sock);
        return -1;
    }

    if (memcmp(response + 28, info_hash, 20) != 0) {
        fprintf(stderr, "bad handshake info_hash\n");
        close(sock);
        return -1;
    }

    printf("peer handshake OK\n");

    printf("peer id: ");
    for (int i = 48; i < 68; i++) {
        unsigned char c = response[i];

        if (c >= 32 && c <= 126)
            putchar(c);
        else
            putchar('.');
    }
    printf("\n");

    return sock; 

}
