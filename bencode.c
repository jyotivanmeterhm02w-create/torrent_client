#include "bencode.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int parse_int(const unsigned char *buf,
                     size_t buf_len,
                     size_t *pos,
                     long long *out);

static BValue *parse_string_value(const unsigned char *buf,
                                  size_t buf_len,
                                  size_t *pos);

static BValue *parse_int_value(const unsigned char *buf,
                               size_t buf_len,
                               size_t *pos);

static BValue *parse_list_value(const unsigned char *buf,
                                size_t buf_len,
                                size_t *pos);

static BValue *parse_dict_value(const unsigned char *buf,
                                size_t buf_len,
                                size_t *pos);

int parse_string(const unsigned char *buf,
                 size_t buf_len,
                 size_t *pos,
                 BString *out)
{
    if (*pos >= buf_len || !isdigit((unsigned char)buf[*pos]))
        return 0;

    size_t len = 0;

    while (*pos < buf_len && isdigit((unsigned char)buf[*pos])) {
        len = len * 10 + (buf[*pos] - '0');
        (*pos)++;
    }

    if (*pos >= buf_len || buf[*pos] != ':')
        return 0;

    (*pos)++;

    if (len > buf_len - *pos)
        return 0;

    out->data = malloc(len + 1);
    if (!out->data)
        return 0;

    memcpy(out->data, buf + *pos, len);
    out->data[len] = '\0';
    out->len = len;

    *pos += len;

    return 1;
}

void free_bstring(BString *s)
{
    if (!s)
        return;

    free(s->data);
    s->data = NULL;
    s->len = 0;
}

static int parse_int(const unsigned char *buf,
                     size_t buf_len,
                     size_t *pos,
                     long long *out)
{
    if (*pos >= buf_len || buf[*pos] != 'i')
        return 0;

    (*pos)++;

    int sign = 1;

    if (*pos < buf_len && buf[*pos] == '-') {
        sign = -1;
        (*pos)++;
    }

    if (*pos >= buf_len || !isdigit((unsigned char)buf[*pos]))
        return 0;

    long long value = 0;

    while (*pos < buf_len && isdigit((unsigned char)buf[*pos])) {
        value = value * 10 + (buf[*pos] - '0');
        (*pos)++;
    }

    if (*pos >= buf_len || buf[*pos] != 'e')
        return 0;

    (*pos)++;

    *out = value * sign;

    return 1;
}

BValue *bdict_get(BValue *dict, const char *key)
{
    if (!dict || dict->type != BENCODE_DICT)
        return NULL;

    size_t key_len = strlen(key);

    for (size_t i = 0; i < dict->dict.count; i++) {
        BString *entry_key = &dict->dict.entries[i].key;

        if (entry_key->len == key_len &&
            memcmp(entry_key->data, key, key_len) == 0) {
            return dict->dict.entries[i].value;
        }
    }

    return NULL;
}

BValue *parse_bvalue(const unsigned char *buf,
                     size_t buf_len,
                     size_t *pos)
{
    if (*pos >= buf_len)
        return NULL;

    size_t start = *pos;

    BValue *v = NULL;

    if (isdigit((unsigned char)buf[*pos])) {
        v = parse_string_value(buf, buf_len, pos);
    } else if (buf[*pos] == 'i') {
        v = parse_int_value(buf, buf_len, pos);
    } else if (buf[*pos] == 'l') {
        v = parse_list_value(buf, buf_len, pos);
    } else if (buf[*pos] == 'd') {
        v = parse_dict_value(buf, buf_len, pos);
    } else {
        return NULL;
    }

    if (!v)
        return NULL;

    v->start = start;
    v->end = *pos;

    return v;
}


static BValue *parse_string_value(const unsigned char *buf,
                                  size_t buf_len,
                                  size_t *pos)
{
    BString s;

    if (!parse_string(buf, buf_len, pos, &s))
        return NULL;

    BValue *v = malloc(sizeof(*v));
    if (!v) {
        free_bstring(&s);
        return NULL;
    }

    v->type = BENCODE_STRING;
    v->string = s;

    return v;
}

static BValue *parse_int_value(const unsigned char *buf,
                               size_t buf_len,
                               size_t *pos)
{
    long long n;

    if (!parse_int(buf, buf_len, pos, &n))
        return NULL;

    BValue *v = malloc(sizeof(*v));
    if (!v)
        return NULL;

    v->type = BENCODE_INT;
    v->integer = n;

    return v;
}

static BValue *parse_list_value(const unsigned char *buf,
                                size_t buf_len,
                                size_t *pos)
{
    if (*pos >= buf_len || buf[*pos] != 'l')
        return NULL;

    (*pos)++;

    BValue *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;

    v->type = BENCODE_LIST;

    while (*pos < buf_len && buf[*pos] != 'e') {
        BValue *item = parse_bvalue(buf, buf_len, pos);

        if (!item) {
            free_bvalue(v);
            return NULL;
        }

        BValue **new_items = realloc(
            v->list.items,
            sizeof(BValue *) * (v->list.count + 1)
        );

        if (!new_items) {
            free_bvalue(item);
            free_bvalue(v);
            return NULL;
        }

        v->list.items = new_items;
        v->list.items[v->list.count] = item;
        v->list.count++;
    }

    if (*pos >= buf_len || buf[*pos] != 'e') {
        free_bvalue(v);
        return NULL;
    }

    (*pos)++;

    return v;
}

static BValue *parse_dict_value(const unsigned char *buf,
                                size_t buf_len,
                                size_t *pos)
{
    if (*pos >= buf_len || buf[*pos] != 'd')
        return NULL;

    (*pos)++;

    BValue *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;

    v->type = BENCODE_DICT;

    while (*pos < buf_len && buf[*pos] != 'e') {
        BString key;

        if (!parse_string(buf, buf_len, pos, &key)) {
            free_bvalue(v);
            return NULL;
        }

        BValue *value = parse_bvalue(buf, buf_len, pos);

        if (!value) {
            free_bstring(&key);
            free_bvalue(v);
            return NULL;
        }

        BDictEntry *new_entries = realloc(
            v->dict.entries,
            sizeof(BDictEntry) * (v->dict.count + 1)
        );

        if (!new_entries) {
            free_bstring(&key);
            free_bvalue(value);
            free_bvalue(v);
            return NULL;
        }

        v->dict.entries = new_entries;
        v->dict.entries[v->dict.count].key = key;
        v->dict.entries[v->dict.count].value = value;
        v->dict.count++;

    }

    if (*pos >= buf_len || buf[*pos] != 'e') {
        free_bvalue(v);
        return NULL;
    }

    (*pos)++;

    return v;
}

void free_bvalue(BValue *v)
{
    if (!v)
        return;

    switch (v->type) {
        case BENCODE_STRING:
            free_bstring(&v->string);
            break;

        case BENCODE_INT:
            break;

        case BENCODE_LIST:
            for (size_t i = 0; i < v->list.count; i++) {
                free_bvalue(v->list.items[i]);
            }

            free(v->list.items);
            break;

        case BENCODE_DICT:
            for (size_t i = 0; i < v->dict.count; i++) {
                free_bstring(&v->dict.entries[i].key);
                free_bvalue(v->dict.entries[i].value);
            }

            free(v->dict.entries);
            break;
    }

    free(v);
}

static void print_indent(int indent)
{
    for (int i = 0; i < indent; i++) {
        putchar(' ');
    }
}

static int bstring_is_printable(const BString *s)
{
    for (size_t i = 0; i < s->len; i++) {
        unsigned char c = s->data[i];

        if (c < 32 || c > 126) {
            return 0;
        }
    }

    return 1;
}

static void print_bstring(const BString *s)
{
    if (!bstring_is_printable(s)) {
        printf("<binary string, len=%zu>", s->len);
        return;
    }

    putchar('"');

    size_t limit = s->len;

    if (limit > 80) {
        limit = 80;
    }

    for (size_t i = 0; i < limit; i++) {
        putchar(s->data[i]);
    }

    if (s->len > limit) {
        printf("...");
    }

    putchar('"');
}

void print_bvalue(const BValue *v, int indent)
{
    if (!v) {
        print_indent(indent);
        printf("null\n");
        return;
    }

    switch (v->type) {
        case BENCODE_STRING:
            print_indent(indent);
            printf("string: ");
            print_bstring(&v->string);
            printf(" len=%zu\n", v->string.len);
            break;

        case BENCODE_INT:
            print_indent(indent);
            printf("int: %lld\n", v->integer);
            break;

        case BENCODE_LIST:
            print_indent(indent);
            printf("list count=%zu\n", v->list.count);

            for (size_t i = 0; i < v->list.count; i++) {
                print_bvalue(v->list.items[i], indent + 2);
            }

            break;

        case BENCODE_DICT:
            print_indent(indent);
            printf("dict count=%zu\n", v->dict.count);

            for (size_t i = 0; i < v->dict.count; i++) {
                print_indent(indent + 2);
                printf("key: ");
                print_bstring(&v->dict.entries[i].key);
                printf("\n");

                print_bvalue(v->dict.entries[i].value, indent + 4);
            }

            break;
    }
}
