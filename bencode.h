#ifndef BENCODE_H
#define BENCODE_H

#include <stddef.h>


typedef struct {
	unsigned char *data;
	size_t len;
} BString;

typedef enum {
	BENCODE_STRING,
	BENCODE_INT,
	BENCODE_LIST,
	BENCODE_DICT
} BType;

typedef struct BValue BValue;

typedef struct {
	BString key;
	BValue *value;
} BDictEntry;

struct BValue {
    BType type;

    size_t start;
    size_t end;

    union {
        BString string;
        long long integer;

        struct {
            BValue **items;
            size_t count;
        } list;

        struct {
            BDictEntry *entries;
            size_t count;
        } dict;
    };
};


BValue *parse_bvalue(const unsigned char *buf,
		size_t buf_len,
		size_t *pos);

void free_bvalue(BValue *v);

int parse_string (const unsigned char *buf,
		size_t buf_len,
		size_t *pos,
		BString *out);

void free_bstring(BString *s);

void print_bvalue(const BValue *v, int indent);


BValue *bdict_get(BValue *dict, const char *key);

#endif
