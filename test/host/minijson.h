/*
 * A minimal JSON reader, used only by the host conformance runner.
 *
 * The firmware itself never parses JSON — the wire format is deterministic
 * CBOR. This exists because the committed conformance vectors describe their
 * expected verdicts in expected.json, and the C implementation has to read the
 * same files the Go and Rust runners read. Pulling in a JSON library for a test
 * harness would be the larger cost.
 *
 * Node storage is a fixed pool: the vectors are small and bounded, and a
 * failure to parse one is a test failure rather than something to recover from.
 */

#ifndef MINIJSON_H
#define MINIJSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MJ_NULL,
    MJ_BOOL,
    MJ_NUMBER,
    MJ_STRING,
    MJ_ARRAY,
    MJ_OBJECT,
} mj_kind_t;

/* Sized for the vectors with room to spare. The longest string in the corpus is
 * a prose "asserts" field, which is what sets MJ_MAX_STR rather than any
 * protocol value. */
#ifndef MJ_MAX_NODES
#define MJ_MAX_NODES 512
#endif
#ifndef MJ_MAX_STR
#define MJ_MAX_STR   1024
#endif

typedef struct mj_node mj_node_t;

struct mj_node {
    mj_kind_t kind;

    bool   b;
    double num;
    char   str[MJ_MAX_STR]; /* the value, for MJ_STRING */
    char   key[MJ_MAX_STR]; /* this node's key, when it is an object member */

    /* Children, as a singly linked list of indices into the pool. */
    int first_child;
    int next_sibling;
};

typedef struct {
    mj_node_t nodes[MJ_MAX_NODES];
    int       used;
    int       root;

    const char *p;
    const char *end;
    char        error[160];
} mj_doc_t;

/* Parse NUL-terminated `text`. Returns false and fills doc->error on failure. */
bool mj_parse(mj_doc_t *doc, const char *text);

const mj_node_t *mj_root(const mj_doc_t *doc);

/* Object member lookup by key; NULL when absent or when `n` is not an object. */
const mj_node_t *mj_get(const mj_doc_t *doc, const mj_node_t *n, const char *key);

size_t           mj_len(const mj_doc_t *doc, const mj_node_t *n);
const mj_node_t *mj_at(const mj_doc_t *doc, const mj_node_t *n, size_t i);

/* Typed accessors with explicit defaults: an absent key is common in these
 * vectors (first_seq is omitted for empty segments) and must not be confused
 * with a present zero. */
const char *mj_str_or(const mj_node_t *n, const char *fallback);
double      mj_num_or(const mj_node_t *n, double fallback);
bool        mj_bool_or(const mj_node_t *n, bool fallback);
bool        mj_is_null(const mj_node_t *n);

/* Iterate object members; each member's key is in node->key. */
const mj_node_t *mj_first(const mj_doc_t *doc, const mj_node_t *n);
const mj_node_t *mj_next(const mj_doc_t *doc, const mj_node_t *n);

#endif /* MINIJSON_H */
