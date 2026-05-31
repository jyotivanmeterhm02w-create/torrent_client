#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include "tracker_udp.h"
#include "bencode.h"
#include <time.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tracker_http.h"
#include "peer.h"

#define BLOCK_SIZE 16384

BValue *pieces = NULL;


static int mkdir_p_for_file(const char *filepath)

{
    char tmp[4096];

    snprintf(tmp, sizeof(tmp), "%s", filepath);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';

            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                perror("mkdir");
                return -1;
            }

            *p = '/';
        }
    }

    return 0;
}




static int write_piece_to_files(BValue *info,
                                uint32_t piece_index,
                                long long piece_length,
                                const unsigned char *piece_buf,
                                long long piece_size)
{
    BValue *name = bdict_get(info, "name");
    BValue *files = bdict_get(info, "files");

    if (!name || name->type != BENCODE_STRING) {
        fprintf(stderr, "torrent name not found\n");
        return -1;
    }

    long long piece_global_start = (long long)piece_index * piece_length;
    long long piece_global_end = piece_global_start + piece_size;

    if (files && files->type == BENCODE_LIST) {
        long long file_global_start = 0;

        for (size_t i = 0; i < files->list.count; i++) {
            BValue *file = files->list.items[i];

            if (!file || file->type != BENCODE_DICT)
                continue;

            BValue *length = bdict_get(file, "length");
            BValue *path = bdict_get(file, "path");

            if (!length || length->type != BENCODE_INT ||
                !path || path->type != BENCODE_LIST) {
                continue;
            }

            long long file_size = length->integer;
            long long file_global_end = file_global_start + file_size;

            if (piece_global_end <= file_global_start ||
                piece_global_start >= file_global_end) {
                file_global_start = file_global_end;
                continue;
            }

            long long overlap_start =
                piece_global_start > file_global_start
                    ? piece_global_start
                    : file_global_start;

            long long overlap_end =
                piece_global_end < file_global_end
                    ? piece_global_end
                    : file_global_end;

            long long bytes_to_write = overlap_end - overlap_start;

            long long src_offset = overlap_start - piece_global_start;
            long long dst_offset = overlap_start - file_global_start;

            char filepath[4096];

            snprintf(filepath,
                     sizeof(filepath),
                     "download/%.*s",
                     (int)name->string.len,
                     name->string.data);

            for (size_t j = 0; j < path->list.count; j++) {
                BValue *part = path->list.items[j];

                if (!part || part->type != BENCODE_STRING)
                    continue;

                size_t used = strlen(filepath);

                snprintf(filepath + used,
                         sizeof(filepath) - used,
                         "/%.*s",
                         (int)part->string.len,
                         part->string.data);
            }

            if (mkdir_p_for_file(filepath) < 0)
                return -1;

            FILE *out = fopen(filepath, "r+b");
            if (!out)
                out = fopen(filepath, "w+b");

            if (!out) {
                perror("fopen output file");
                return -1;
            }

            if (fseek(out, dst_offset, SEEK_SET) != 0) {
                perror("fseek output file");
                fclose(out);
                return -1;
            }

            size_t written = fwrite(piece_buf + src_offset,
                                    1,
                                    (size_t)bytes_to_write,
                                    out);

            fclose(out);

            if (written != (size_t)bytes_to_write) {
                fprintf(stderr, "short write\n");
                return -1;
            }

            printf("wrote %lld bytes to %s at offset %lld\n",
                   bytes_to_write,
                   filepath,
                   dst_offset);

            file_global_start = file_global_end;
        }

        return 0;
    }

    BValue *length = bdict_get(info, "length");

    if (!length || length->type != BENCODE_INT) {
        fprintf(stderr, "single-file length not found\n");
        return -1;
    }

    char filepath[4096];

    snprintf(filepath,
             sizeof(filepath),
             "download/%.*s",
             (int)name->string.len,
             name->string.data);

    if (mkdir_p_for_file(filepath) < 0)
        return -1;

    FILE *out = fopen(filepath, "r+b");
    if (!out)
        out = fopen(filepath, "w+b");

    if (!out) {
        perror("fopen output file");
        return -1;
    }

    if (fseek(out, piece_global_start, SEEK_SET) != 0) {
        perror("fseek output file");
        fclose(out);
        return -1;
    }

    size_t written = fwrite(piece_buf, 1, (size_t)piece_size, out);

    fclose(out);

    if (written != (size_t)piece_size) {
        fprintf(stderr, "short write\n");
        return -1;
    }

    printf("wrote piece %u to %s\n", piece_index, filepath);

    return 0;
}




static int bitfield_has_piece(const unsigned char *bitfield,
                              uint32_t bitfield_len,
                              uint32_t piece_index)
{
    uint32_t byte_index = piece_index / 8;
    uint32_t bit_index = 7 - (piece_index % 8);

    if (byte_index >= bitfield_len)
        return 0;

    return (bitfield[byte_index] & (1 << bit_index)) != 0;
}


static uint32_t read_u32_be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void announce_tracker_url(const char *url,
                                 const unsigned char info_hash[20],
                                 const unsigned char peer_id[20],
                                 long long left,
                                 Peer *peers,
                                 size_t max_peers,
                                 size_t *peers_count)
{
    if (!url || !*url)
        return;

    if (*peers_count >= max_peers) {
        printf("peer array full, skip tracker: %s\n", url);
        return;
    }

    if (starts_with(url, "udp://")) {
        printf("trying torrent UDP tracker: %s\n", url);

        tracker_udp_announce(url,
                             info_hash,
                             peer_id,
                             left,
                             peers,
                             max_peers,
                             peers_count);
    }
    else if (starts_with(url, "http://") || starts_with(url, "https://")) {
        printf("trying torrent HTTP tracker: %s\n", url);

        tracker_http_announce(url,
                              info_hash,
                              peer_id,
                              left,
                              peers,
                              max_peers,
                              peers_count);
    }
    else {
        printf("unsupported tracker scheme: %s\n", url);
    }
}

static void announce_tracker_bvalue(BValue *tracker,
                                    const unsigned char info_hash[20],
                                    const unsigned char peer_id[20],
                                    long long left,
                                    Peer *peers,
                                    size_t max_peers,
                                    size_t *peers_count)
{
    if (!tracker || tracker->type != BENCODE_STRING)
        return;

    char *url = malloc(tracker->string.len + 1);
    if (!url)
        return;

    memcpy(url, tracker->string.data, tracker->string.len);
    url[tracker->string.len] = '\0';

    announce_tracker_url(url,
                         info_hash,
                         peer_id,
                         left,
                         peers,
                         max_peers,
                         peers_count);

    free(url);
}

static void announce_from_torrent_trackers(BValue *root,
                                           const unsigned char info_hash[20],
                                           const unsigned char peer_id[20],
                                           long long left,
                                           Peer *peers,
                                           size_t max_peers,
                                           size_t *peers_count)
{
    BValue *announce = bdict_get(root, "announce");

    if (announce && announce->type == BENCODE_STRING) {
        printf("\n=== announce ===\n");

        announce_tracker_bvalue(announce,
                                info_hash,
                                peer_id,
                                left,
                                peers,
                                max_peers,
                                peers_count);
    }

    BValue *announce_list = bdict_get(root, "announce-list");

    if (announce_list && announce_list->type == BENCODE_LIST) {
        printf("\n=== announce-list ===\n");

        for (size_t i = 0; i < announce_list->list.count; i++) {
            BValue *tier = announce_list->list.items[i];

            if (!tier)
                continue;

            printf("tier %zu\n", i);

            if (tier->type == BENCODE_LIST) {
                for (size_t j = 0; j < tier->list.count; j++) {
                    BValue *tracker = tier->list.items[j];

                    announce_tracker_bvalue(tracker,
                                            info_hash,
                                            peer_id,
                                            left,
                                            peers,
                                            max_peers,
                                            peers_count);
                }
            }
            else if (tier->type == BENCODE_STRING) {
                announce_tracker_bvalue(tier,
                                        info_hash,
                                        peer_id,
                                        left,
                                        peers,
                                        max_peers,
                                        peers_count);
            }
        }
    }
}


int main(void)
{


	FILE *fp = fopen ("res/torrent_file", "rb");
	if (!fp) {
		perror ("res/torrent_file");
		return 1;
	};

	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	rewind (fp);

	unsigned char *buf = malloc (size);
	if (!buf) {
		fclose (fp);
		return 1;
	}

	size_t check_bytes = fread(buf, 1, size, fp);
	if (check_bytes != (size_t)size) {
		fclose (fp);
		free (buf);
		return 1;
	}

	fclose(fp);

	printf ("file size: %ld\n", size);
	printf ("1 letter: %c\n", buf[0]);

	if (buf[0] != 'd') {
		fprintf(stderr, "Not a bencode dictionary\n");
		free (buf);
		return 1;
	}

	size_t pos = 0;
	BValue *root = parse_bvalue(buf, size, &pos);

	if (!root) {
		fprintf(stderr, "parse error at pos %zu\n", pos);
		free(buf);
		return 1;
	}
	unsigned char info_hash[SHA_DIGEST_LENGTH];

	int have_info_hash = 0;

	long long total_length = 0;

	long long piece_len_value = 0;


	printf("parsed ok, final pos = %zu\n", pos);

	BValue *announce = bdict_get(root, "announce");

	if (announce && announce->type == BENCODE_STRING) {
		printf("announce: %.*s\n",
		(int)announce->string.len,
		announce->string.data);
	} 
	else {
		printf("announce not found\n");
	}

	BValue *announce_list = bdict_get(root, "announce-list");

	if (announce_list && announce_list->type == BENCODE_LIST) {

		printf("announce-list found, tiers: %zu\n", announce_list->list.count);

		for (size_t i = 0; i < announce_list->list.count; i++) {
			BValue *tier = announce_list->list.items[i];
			if (!tier || tier->type != BENCODE_LIST) {
				printf("tier %zu is not a list\n", i);
				continue;
			}

			printf("tier %zu, trackers: %zu\n", i, tier->list.count);
			for (size_t j = 0; j < tier->list.count; j++) {
				BValue *tracker = tier->list.items[j];
				if (!tracker || tracker->type != BENCODE_STRING) {
					printf("bad tracker at tier %zu index %zu\n", i, j);
					continue;
				}
				printf("tracker: %.*s\n",
						(int)tracker->string.len,
						tracker->string.data);
			}
		}
	}

	else 
		printf("announce-list not found\n");


	BValue *info = bdict_get(root, "info");

	if (info && info->type == BENCODE_DICT) {
		printf("info found, keys: %zu\n", info->dict.count);
		printf("info span: %zu..%zu, len=%zu\n",
				info->start, info->end, info->end - info->start);

		SHA1(buf + info->start,
				info->end - info->start,
				info_hash);

		have_info_hash = 1;

		printf("info_hash: ");

		for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
			printf("%02x", info_hash[i]);
		}

		printf("\n");
	} 
	else {
		printf("info not found\n");
	}

	printf("my info_hash: ");

	for (int i = 0; i < 20; i++) {
		printf("%02x", info_hash[i]);
	}

	printf("\n");

	unsigned char peer_id[20];

	srand((unsigned int)time(NULL));

	memcpy(peer_id, "-GT0001-", 8);


	for (int i = 8; i < 20; i++) {
		peer_id[i] = '0' + rand() % 10;
	}

	printf("peer_id: ");

	for (int i = 0; i < 20; i++) {
		putchar(peer_id[i]);
	}

	printf("\n");



	BValue *name = bdict_get(info, "name");

	if (name && name->type == BENCODE_STRING) {
		printf("name: %.*s\n",
		(int)name->string.len,
		name->string.data);
	} 
	else {
		printf("name not found\n");
	}

	BValue *piece_length = bdict_get(info, "piece length");

	if (piece_length && piece_length->type == BENCODE_INT) {

		piece_len_value = piece_length->integer;
		printf("piece length: %lld\n", piece_length->integer);
	} 
	else {
		printf("piece length not found\n");
	}

	BValue *pieces = bdict_get(info, "pieces");

	if (pieces && pieces->type == BENCODE_STRING) {
		printf("pieces length: %zu\n", pieces->string.len);
		printf("pieces count: %zu\n", pieces->string.len / 20);
	}
	else {
		printf("pieces not found\n");
	}

	BValue *files = bdict_get(info, "files");

	if (files && files->type == BENCODE_LIST) {
		total_length = 0;
		printf("files count: %zu\n", files->list.count);

		for (size_t i = 0; i < files->list.count; i++) {

			BValue *file = files->list.items[i];
			if (!file || file->type != BENCODE_DICT) {
				printf("bad file entry at index %zu\n", i);
				continue;
			}

			BValue *length = bdict_get(file, "length");
			BValue *path = bdict_get(file, "path");

			if (!length || length->type != BENCODE_INT) {

				printf("file %zu: length not found\n", i);

				continue;
			}

			total_length += length->integer;

			if (i < 10) {

				printf("file %zu: %lld bytes ", i, length->integer);
				if (path && path->type == BENCODE_LIST) {
					printf("path: ");
					for (size_t j = 0; j < path->list.count; j++) {
						BValue *part = path->list.items[j];
						if (part && part->type == BENCODE_STRING) {
							printf("%.*s",
								(int)part->string.len,
								part->string.data);

							if (j + 1 < path->list.count) {
								printf("/");
							}
						}
					}
				}

				printf("\n");
			}
		}
		printf("total length: %lld\n", total_length);
	} 
	
	else {
		printf("files not found, maybe single-file torrent\n");
	}

	Peer peers[256];

	size_t peers_count = 0;

	if (have_info_hash && total_length > 0) {
		announce_from_torrent_trackers(root,
                                   info_hash,
                                   peer_id,
                                   total_length,
                                   peers,
                                   256,
                                   &peers_count);
	}

	printf("collected peers: %zu\n", peers_count);


	printf("collected peers: %zu\n", peers_count);

uint32_t pieces_count = pieces->string.len / 20;

unsigned char *downloaded_map = calloc(pieces_count, 1);
if (!downloaded_map) {
    fprintf(stderr, "calloc downloaded_map failed\n");
    free_bvalue(root);
    free(buf);
    return 1;
}

uint32_t downloaded_pieces = 0;

for (size_t p = 0; p < peers_count; p++) {
    if (downloaded_pieces == pieces_count)
        break;

    printf("trying peer %s:%u\n", peers[p].ip, peers[p].port);

    int peer_sock = peer_connect_handshake(peers[p].ip,
                                           peers[p].port,
                                           info_hash,
                                           peer_id);

    if (peer_sock < 0)
        continue;

    printf("working peer found\n");
    printf("ready to communicate with peer socket: %d\n", peer_sock);

    peer_set_timeout(peer_sock, 30);

    unsigned char *peer_has_piece = calloc(pieces_count, 1);
    if (!peer_has_piece) {
        fprintf(stderr, "calloc peer_has_piece failed\n");
        close(peer_sock);
        continue;
    }

    if (peer_send_interested(peer_sock) < 0) {
        fprintf(stderr, "failed to send interested\n");
        free(peer_has_piece);
        close(peer_sock);
        continue;
    }

    printf("sent interested\n");

    unsigned char msg_id;
    unsigned char payload[65536];
    uint32_t payload_len;

    int got_unchoke = 0;

    while (1) {
        if (peer_read_message(peer_sock,
                              &msg_id,
                              payload,
                              sizeof(payload),
                              &payload_len) < 0) {
            printf("peer failed before unchoke\n");
            break;
        }

        if (msg_id == 255) {
            printf("received keep-alive\n");
            continue;
        }

        printf("received message id=%u payload_len=%u\n",
               msg_id,
               payload_len);

        if (msg_id == 5) {
            printf("received bitfield\n");

            for (uint32_t i = 0; i < pieces_count; i++) {
                if (bitfield_has_piece(payload, payload_len, i)) {
                    peer_has_piece[i] = 1;
                }
            }

            continue;
        }

        if (msg_id == 4) {
            if (payload_len >= 4) {
                uint32_t have_piece = read_u32_be(payload);

                if (have_piece < pieces_count) {
                    peer_has_piece[have_piece] = 1;
                }

                printf("peer have piece %u\n", have_piece);
            }

            continue;
        }

        if (msg_id == 0) {
            printf("peer choked us before download\n");
            continue;
        }

        if (msg_id == 1) {
            printf("peer unchoked us, now we can request pieces\n");
            got_unchoke = 1;
            break;
        }
    }

    if (!got_unchoke) {
        free(peer_has_piece);
        close(peer_sock);
        continue;
    }

    int peer_failed = 0;
    int peer_choked = 0;

    for (uint32_t target_piece = 0; target_piece < pieces_count; target_piece++) {
        if (downloaded_map[target_piece])
            continue;

        if (!peer_has_piece[target_piece])
            continue;

        if (peer_failed)
            break;

        printf("selected piece %u\n", target_piece);

        long long torrent_piece_length = piece_len_value;
        long long piece_size = torrent_piece_length;

        if ((long long)(target_piece + 1) * torrent_piece_length > total_length) {
            piece_size = total_length - (long long)target_piece * torrent_piece_length;
        }

        unsigned char *piece_buf = malloc((size_t)piece_size);
        if (!piece_buf) {
            fprintf(stderr, "malloc piece_buf failed\n");
            peer_failed = 1;
            break;
        }

        uint32_t downloaded = 0;

        while (downloaded < piece_size) {
            if (peer_choked) {
                printf("peer choked us, waiting for unchoke\n");

                if (peer_read_message(peer_sock,
                                      &msg_id,
                                      payload,
                                      sizeof(payload),
                                      &payload_len) < 0) {
                    printf("failed to read peer message while choked\n");
                    peer_failed = 1;
                    goto piece_done;
                }

                if (msg_id == 255) {
                    printf("received keep-alive\n");
                    continue;
                }

                printf("received message id=%u payload_len=%u\n",
                       msg_id,
                       payload_len);

                if (msg_id == 1) {
                    peer_choked = 0;
                    printf("peer unchoked us again\n");
                }

                if (msg_id == 0) {
                    printf("peer still choked us\n");
                }

                continue;
            }

            uint32_t request_len = BLOCK_SIZE;

            if ((long long)downloaded + request_len > piece_size) {
                request_len = (uint32_t)(piece_size - downloaded);
            }

            printf("about to send request\n");

            if (peer_send_request(peer_sock,
                                  target_piece,
                                  downloaded,
                                  request_len) < 0) {
                printf("failed to send request\n");
                peer_failed = 1;
                goto piece_done;
            }

            printf("sent request: piece=%u begin=%u length=%u\n",
                   target_piece,
                   downloaded,
                   request_len);

            while (1) {
                if (peer_read_message(peer_sock,
                                      &msg_id,
                                      payload,
                                      sizeof(payload),
                                      &payload_len) < 0) {
                    printf("failed to read peer message after request\n");
                    peer_failed = 1;
                    goto piece_done;
                }

                if (msg_id == 255) {
                    printf("received keep-alive\n");
                    continue;
                }

                printf("received message id=%u payload_len=%u\n",
                       msg_id,
                       payload_len);

                if (msg_id == 0) {
                    printf("peer choked us\n");
                    peer_failed = 1;
                    goto piece_done;
                }

                if (msg_id == 1) {
                    peer_choked = 0;
                    printf("peer unchoked us\n");
                    continue;
                }

                if (msg_id == 4) {
                    if (payload_len >= 4) {
                        uint32_t have_piece = read_u32_be(payload);

                        if (have_piece < pieces_count) {
                            peer_has_piece[have_piece] = 1;
                        }

                        printf("peer have piece %u\n", have_piece);
                    }

                    continue;
                }

                if (msg_id == 5) {
                    printf("received bitfield\n");

                    for (uint32_t i = 0; i < pieces_count; i++) {
                        if (bitfield_has_piece(payload, payload_len, i)) {
                            peer_has_piece[i] = 1;
                        }
                    }

                    continue;
                }

                if (msg_id != 7) {
                    continue;
                }

                if (payload_len < 8) {
                    printf("bad piece message\n");
                    peer_failed = 1;
                    goto piece_done;
                }

                uint32_t got_piece = read_u32_be(payload);
                uint32_t got_begin = read_u32_be(payload + 4);
                uint32_t block_len = payload_len - 8;

                printf("received block: piece=%u begin=%u block_len=%u\n",
                       got_piece,
                       got_begin,
                       block_len);

                if (got_piece != target_piece) {
                    printf("wrong piece, ignoring\n");
                    continue;
                }

                if (got_begin != downloaded) {
                    printf("unexpected begin: got %u expected %u\n",
                           got_begin,
                           downloaded);
                    continue;
                }

                if ((long long)got_begin + block_len > piece_size) {
                    printf("block out of range\n");
                    peer_failed = 1;
                    goto piece_done;
                }

                memcpy(piece_buf + got_begin, payload + 8, block_len);
                downloaded += block_len;

                printf("piece progress: %u / %lld\n",
                       downloaded,
                       piece_size);

                break;
            }
        }

piece_done:

        if (downloaded == piece_size) {
            printf("piece %u downloaded completely\n", target_piece);

            unsigned char calc_hash[20];

            SHA1(piece_buf, (size_t)piece_size, calc_hash);

            const unsigned char *expected_hash =
                pieces->string.data + target_piece * 20;

            if (memcmp(calc_hash, expected_hash, 20) == 0) {
                printf("piece hash OK\n");

                if (write_piece_to_files(info,
                                         target_piece,
                                         piece_len_value,
                                         piece_buf,
                                         piece_size) < 0) {
                    printf("failed to write piece to files\n");
                    peer_failed = 1;
                } else {
                    printf("piece written to files\n");

                    downloaded_map[target_piece] = 1;
                    downloaded_pieces++;

                    printf("progress: %u / %u pieces downloaded\n",
                           downloaded_pieces,
                           pieces_count);
                }
            } else {
                printf("piece hash FAILED\n");
                peer_failed = 1;
            }
        } else {
            printf("piece %u incomplete: %u / %lld\n",
                   target_piece,
                   downloaded,
                   piece_size);
        }

        free(piece_buf);

        if (peer_failed) {
            printf("peer failed, trying next peer\n");
            break;
        }

        if (downloaded_pieces == pieces_count)
            break;
    }

    free(peer_has_piece);
    close(peer_sock);
}

if (downloaded_pieces == pieces_count) {
    printf("download complete\n");
} else {
    printf("download incomplete: %u / %u pieces downloaded\n",
           downloaded_pieces,
           pieces_count);
}

free(downloaded_map);

free_bvalue(root);
free(buf);

return 0;
}
