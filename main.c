#include "bencode.h"
#include <time.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


int main(void)
{
	FILE *fp = fopen ("res/gtorr_net_fh5-portable.torrent", "rb");
	if (!fp) {
		perror ("res/gtorr_net_fh5-portable.torrent");
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
	if (check_bytes != size) {
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

	BValue *info = bdict_get(root, "info");

	if (info && info->type == BENCODE_DICT) {
		printf("info found, keys: %zu\n", info->dict.count);
		printf("info span: %zu..%zu, len=%zu\n",
				info->start, info->end, info->end - info->start);
		unsigned char info_hash[SHA_DIGEST_LENGTH];

		SHA1(buf + info->start,
				info->end - info->start,
				info_hash);

		printf("info_hash: ");

		for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
			printf("%02x", info_hash[i]);
		}

		printf("\n");
	} 
	else {
		printf("info not found\n");
	}

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
		long long total_length = 0;
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
	
	//print_bvalue(root, 0);


	free_bvalue(root);
	free (buf);

	return 0;






	
}
