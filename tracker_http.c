#include "tracker_http.h"
#include "bencode.h"
#include "peer.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



static void save_compact_peers(const BString *peers_str,
                               Peer *out_peers,
                               size_t max_peers,
                               size_t *out_peers_count)
{
    if (!peers_str || peers_str->len == 0) {
        printf("no peers\n");
        return;
    }

    if (peers_str->len % 6 != 0) {
        printf("bad compact peers length: %zu\n", peers_str->len);
        return;
    }

    size_t count = peers_str->len / 6;

    printf("peers:\n");

    for (size_t i = 0; i < count; i++) {
        const unsigned char *p = peers_str->data + i * 6;

        unsigned int ip1 = p[0];
        unsigned int ip2 = p[1];
        unsigned int ip3 = p[2];
        unsigned int ip4 = p[3];

        unsigned int port = ((unsigned int)p[4] << 8) | p[5];

        printf("  %u.%u.%u.%u:%u\n",
               ip1, ip2, ip3, ip4, port);

        if (ip1 == 127)
            continue;

        if (*out_peers_count >= max_peers)
            continue;

        snprintf(out_peers[*out_peers_count].ip,
                 sizeof(out_peers[*out_peers_count].ip),
                 "%u.%u.%u.%u",
                 ip1, ip2, ip3, ip4);

        out_peers[*out_peers_count].port = (unsigned short)port;
        (*out_peers_count)++;
    }
}



typedef struct {
    unsigned char *data;
    size_t size;
} ResponseBuffer;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    ResponseBuffer *buf = userp;

    unsigned char *new_data = realloc(buf->data, buf->size + total);
    if (!new_data)
        return 0;

    buf->data = new_data;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;

    return total;
}
int tracker_http_announce(const char *tracker_url,
                          const unsigned char info_hash[20],
                          const unsigned char peer_id[20],
                          long long left,
                          Peer *out_peers,
                          size_t max_peers,
                          size_t *out_peers_count)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        return 0;
    }

    char *encoded_info_hash = curl_easy_escape(curl, (const char *)info_hash, 20);
    char *encoded_peer_id = curl_easy_escape(curl, (const char *)peer_id, 20);

    if (!encoded_info_hash || !encoded_peer_id) {
        fprintf(stderr, "curl_easy_escape failed\n");

        if (encoded_info_hash)
            curl_free(encoded_info_hash);

        if (encoded_peer_id)
            curl_free(encoded_peer_id);

        curl_easy_cleanup(curl);
        return 0;
    }

    char url[4096];

    const char *sep = strchr(tracker_url, '?') ? "&" : "?";


    snprintf(url,
         sizeof(url),
         "%s%sinfo_hash=%s"
         "&peer_id=%s"
         "&port=6881"
         "&uploaded=0"
         "&downloaded=0"
         "&left=%lld"
         "&compact=1"
         "&numwant=50"
         "&event=started",
         tracker_url,
         sep,
         encoded_info_hash,
         encoded_peer_id,
         left);

    printf("tracker request:\n%s\n", url);

    ResponseBuffer response;
    response.data = NULL;
    response.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));

        free(response.data);
        curl_free(encoded_info_hash);
        curl_free(encoded_peer_id);
        curl_easy_cleanup(curl);

        return 0;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    printf("tracker HTTP code: %ld\n", http_code);
    printf("tracker response size: %zu bytes\n", response.size);

    if (response.size > 0) {
        printf("tracker response first byte: %c\n", response.data[0]);
    }


    if (http_code == 200 && response.size > 0 && response.data[0] == 'd') {
    size_t pos = 0;

    BValue *tracker_resp = parse_bvalue(response.data, response.size, &pos);

    if (!tracker_resp) {
        printf("failed to parse tracker response\n");
    } else {
        printf("tracker response parsed, pos=%zu\n", pos);

        BValue *failure = bdict_get(tracker_resp, "failure reason");
        if (failure && failure->type == BENCODE_STRING) {
            printf("tracker failure: %.*s\n",
                   (int)failure->string.len,
                   failure->string.data);
        }

        BValue *interval = bdict_get(tracker_resp, "interval");
        if (interval && interval->type == BENCODE_INT) {
            printf("tracker interval: %lld\n", interval->integer);
        }

       BValue *peers = bdict_get(tracker_resp, "peers");


       BValue *complete = bdict_get(tracker_resp, "complete");
if (complete && complete->type == BENCODE_INT) {
    printf("tracker complete/seeders: %lld\n", complete->integer);
}

BValue *incomplete = bdict_get(tracker_resp, "incomplete");
if (incomplete && incomplete->type == BENCODE_INT) {
    printf("tracker incomplete/leechers: %lld\n", incomplete->integer);
}

if (failure && failure->type == BENCODE_STRING) {
    printf("tracker failure: %.*s\n",
           (int)failure->string.len,
           failure->string.data);
}

BValue *warning = bdict_get(tracker_resp, "warning message");
if (warning && warning->type == BENCODE_STRING) {
    printf("tracker warning: %.*s\n",
           (int)warning->string.len,
           warning->string.data);
}
if (peers && peers->type == BENCODE_STRING) {
    printf("peers compact length: %zu\n", peers->string.len);
    printf("peers count: %zu\n", peers->string.len / 6);

    save_compact_peers(&peers->string,
                   out_peers,
                   max_peers,
                   out_peers_count);
}
        free_bvalue(tracker_resp);
    }
} else {
    printf("tracker response is not bencode, skipping parse\n");
}

    free(response.data);
    curl_free(encoded_info_hash);
    curl_free(encoded_peer_id);
    curl_easy_cleanup(curl);

    return 1;
}
