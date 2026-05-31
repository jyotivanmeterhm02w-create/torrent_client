#ifndef PEER_H
#define PEER_H

#include <stdint.h>



int peer_set_timeout(int sock, int seconds);
typedef struct {
    char ip[16];
    unsigned short port;
} Peer;

int peer_connect_handshake(const char *ip,
                           unsigned short port,
                           const unsigned char info_hash[20],
                           const unsigned char peer_id[20]);

int peer_send_interested(int sock);

int peer_read_message(int sock,
                      unsigned char *msg_id,
                      unsigned char *payload,
                      uint32_t payload_capacity,
                      uint32_t *payload_len);

int peer_send_request(int sock,
                      uint32_t piece_index,
                      uint32_t begin,
                      uint32_t length);

#endif
