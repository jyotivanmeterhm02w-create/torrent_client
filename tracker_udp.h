#ifndef TRACKER_UDP_H
#define TRACKER_UDP_H

#include <stddef.h>
#include "peer.h"

int tracker_udp_announce(const char *tracker_url,
                         const unsigned char info_hash[20],
                         const unsigned char peer_id[20],
                         long long left,
                         Peer *out_peers,
                         size_t max_peers,
                         size_t *out_peers_count);

#endif
