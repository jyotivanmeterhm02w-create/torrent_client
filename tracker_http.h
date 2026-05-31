#ifndef TRACKER_HTTP_H
#define TRACKER_HTTP_H

#include <stddef.h>
#include "peer.h"

int tracker_http_announce(const char *tracker_url,
                          const unsigned char info_hash[20],
                          const unsigned char peer_id[20],
                          long long left,
                          Peer *peers,
                          size_t max_peers,
                          size_t *peers_count);

#endif
