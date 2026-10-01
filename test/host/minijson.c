#include "minijson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_value(mj_doc_t *d);

/* Keeps the first failure: later ones are usually cascades from it. */
static void fail(mj_doc_t *d, const char *what)
{
    if (d->error[0] == '\0') snprintf(d->error, sizeof(d->error), "%s", what);
}

static int alloc_node(mj_doc_t *d, mj_kind_t kind)
{
    if (d->used >= MJ_MAX_NODES) {
        fail(d, "JSON node pool exhausted");
        return -1;
    }

    int idx = d->used++;
    mj_node_t *n = &d->nodes[idx];

    n->kind = kind;
    n->b = false;
    n->num = 0;
    n->str[0] = '\0';
    n->key[0] = '\0';
    n->first_child = -1;
    n->next_sibling = -1;

    return idx;
}

static void skip_ws(mj_doc_t *d)
{
    while (d->p < d->end &&
           (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r')) {
        d->p++;
    }
}

static bool literal(mj_doc_t *d, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(d->end - d->p) < n || memcmp(d->p, lit, n) != 0) return false;
    d->p += n;
    return true;
}

/* Reads a JSON string into `out`. Handles the escapes these vectors can
 * contain; \u is decoded only for the ASCII range, which is all that appears. */
static bool parse_string_into(mj_doc_t *d, char *out, size_t cap)
{
    if (d->p >= d->end || *d->p != '"') {
        fail(d, "expected a string");
        return false;
    }
    d->p++;

    size_t len = 0;
    while (d->p < d->end && *d->p != '"') {
        char c = *d->p++;

        if (c == '\\') {
            if (d->p >= d->end) {
                fail(d, "truncated escape sequence");
                return false;
            }
            char e = *d->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '/': c = '/';  break;
            case '"': c = '"';  break;
            case '\\': c = '\\'; break;
            case 'u': {
                if (d->end - d->p < 4) {
                    fail(d, "truncated \\u escape");
                    return false;
                }
                char hex[5] = { d->p[0], d->p[1], d->p[2], d->p[3], '\0' };
                d->p += 4;
                long v = strtol(hex, NULL, 16);
                /* Non-ASCII would need UTF-8 encoding; the vectors contain
                 * none, so refuse rather than emit something wrong. */
                if (v > 0x7F) {
                    fail(d, "non-ASCII \\u escape is unsupported");
                    return false;
                }
                c = (char)v;
                break;
            }
            default:
                fail(d, "unknown escape sequence");
                return false;
            }
        }

        if (len + 1 >= cap) {
            fail(d, "string longer than MJ_MAX_STR");
            return false;
        }
        out[len++] = c;
    }

    if (d->p >= d->end) {
        fail(d, "unterminated string");
        return false;
    }
    d->p++; /* closing quote */

    out[len] = '\0';
    return true;
}

static int parse_object(mj_doc_t *d)
{
    int idx = alloc_node(d, MJ_OBJECT);
    if (idx < 0) return -1;

    d->p++; /* '{' */
    skip_ws(d);

    if (d->p < d->end && *d->p == '}') {
        d->p++;
        return idx;
    }

    int last = -1;
    for (;;) {
        skip_ws(d);

        char key[MJ_MAX_STR];
        if (!parse_string_into(d, key, sizeof(key))) return -1;

        skip_ws(d);
        if (d->p >= d->end || *d->p != ':') {
            fail(d, "expected ':' after an object key");
            return -1;
        }
        d->p++;
        skip_ws(d);

        int child = parse_value(d);
        if (child < 0) return -1;

        /* The key is recorded on the child, in its own field: a string value
         * must not be clobbered by the key that introduced it. */
        memcpy(d->nodes[child].key, key, sizeof(key));

        if (last < 0) d->nodes[idx].first_child = child;
        else          d->nodes[last].next_sibling = child;
        last = child;

        skip_ws(d);
        if (d->p < d->end && *d->p == ',') {
            d->p++;
            continue;
        }
        if (d->p < d->end && *d->p == '}') {
            d->p++;
            return idx;
        }
        fail(d, "expected ',' or '}' in an object");
        return -1;
    }
}

static int parse_array(mj_doc_t *d)
{
    int idx = alloc_node(d, MJ_ARRAY);
    if (idx < 0) return -1;

    d->p++; /* '[' */
    skip_ws(d);

    if (d->p < d->end && *d->p == ']') {
        d->p++;
        return idx;
    }

    int last = -1;
    for (;;) {
        skip_ws(d);

        int child = parse_value(d);
        if (child < 0) return -1;

        if (last < 0) d->nodes[idx].first_child = child;
        else          d->nodes[last].next_sibling = child;
        last = child;

        skip_ws(d);
        if (d->p < d->end && *d->p == ',') {
            d->p++;
            continue;
        }
        if (d->p < d->end && *d->p == ']') {
            d->p++;
            return idx;
        }
        fail(d, "expected ',' or ']' in an array");
        return -1;
    }
}

static int parse_value(mj_doc_t *d)
{
    skip_ws(d);
    if (d->p >= d->end) {
        fail(d, "unexpected end of input");
        return -1;
    }

    char c = *d->p;

    if (c == '{') return parse_object(d);
    if (c == '[') return parse_array(d);

    if (c == '"') {
        int idx = alloc_node(d, MJ_STRING);
        if (idx < 0) return -1;

        if (!parse_string_into(d, d->nodes[idx].str, MJ_MAX_STR)) return -1;
        return idx;
    }

    if (c == 't' || c == 'f') {
        int idx = alloc_node(d, MJ_BOOL);
        if (idx < 0) return -1;
        if (literal(d, "true"))       d->nodes[idx].b = true;
        else if (literal(d, "false")) d->nodes[idx].b = false;
        else {
            fail(d, "malformed boolean");
            return -1;
        }
        return idx;
    }

    if (c == 'n') {
        int idx = alloc_node(d, MJ_NULL);
        if (idx < 0) return -1;
        if (!literal(d, "null")) {
            fail(d, "malformed null");
            return -1;
        }
        return idx;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        int idx = alloc_node(d, MJ_NUMBER);
        if (idx < 0) return -1;

        char *endp = NULL;
        double v = strtod(d->p, &endp);
        if (endp == d->p) {
            fail(d, "malformed number");
            return -1;
        }
        d->p = endp;
        d->nodes[idx].num = v;
        return idx;
    }

    fail(d, "unexpected character");
    return -1;
}

bool mj_parse(mj_doc_t *doc, const char *text)
{
    memset(doc, 0, sizeof(*doc));
    doc->used = 0;
    doc->root = -1;
    doc->p = text;
    doc->end = text + strlen(text);
    doc->error[0] = '\0';

    doc->root = parse_value(doc);
    if (doc->root < 0) return false;

    skip_ws(doc);
    if (doc->p != doc->end) {
        fail(doc, "trailing content after the top-level value");
        return false;
    }
    return true;
}

const mj_node_t *mj_root(const mj_doc_t *doc)
{
    if (doc->root < 0) return NULL;
    return &doc->nodes[doc->root];
}

const mj_node_t *mj_get(const mj_doc_t *doc, const mj_node_t *n, const char *key)
{
    if (n == NULL || n->kind != MJ_OBJECT) return NULL;

    for (int i = n->first_child; i >= 0; i = doc->nodes[i].next_sibling) {
        if (strcmp(doc->nodes[i].key, key) == 0) return &doc->nodes[i];
    }
    return NULL;
}

size_t mj_len(const mj_doc_t *doc, const mj_node_t *n)
{
    if (n == NULL) return 0;

    size_t count = 0;
    for (int i = n->first_child; i >= 0; i = doc->nodes[i].next_sibling) count++;
    return count;
}

const mj_node_t *mj_at(const mj_doc_t *doc, const mj_node_t *n, size_t want)
{
    if (n == NULL) return NULL;

    size_t idx = 0;
    for (int i = n->first_child; i >= 0; i = doc->nodes[i].next_sibling) {
        if (idx++ == want) return &doc->nodes[i];
    }
    return NULL;
}

const char *mj_str_or(const mj_node_t *n, const char *fallback)
{
    if (n == NULL || n->kind != MJ_STRING) return fallback;
    return n->str;
}

double mj_num_or(const mj_node_t *n, double fallback)
{
    if (n == NULL || n->kind != MJ_NUMBER) return fallback;
    return n->num;
}

bool mj_bool_or(const mj_node_t *n, bool fallback)
{
    if (n == NULL || n->kind != MJ_BOOL) return fallback;
    return n->b;
}

bool mj_is_null(const mj_node_t *n)
{
    return n != NULL && n->kind == MJ_NULL;
}

const mj_node_t *mj_first(const mj_doc_t *doc, const mj_node_t *n)
{
    if (n == NULL || n->first_child < 0) return NULL;
    return &doc->nodes[n->first_child];
}

const mj_node_t *mj_next(const mj_doc_t *doc, const mj_node_t *n)
{
    if (n == NULL || n->next_sibling < 0) return NULL;
    return &doc->nodes[n->next_sibling];
}
