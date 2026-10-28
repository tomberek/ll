/* M195: see include/asnix/art_sym.h for the full rationale and measured
 * numbers. Variable-size-per-kind nodes (Leaf/Node4/Node16/Node48/Node256
 * each their own struct, not one union sized for the largest), mutable
 * in-place insertion (no copy-on-write -- nothing needs an old root to
 * stay valid), allocated via asnix_arena_alloc (never individually
 * freed, same as every other scratch allocation in this codebase).
 */
#include "asnix/art_sym.h"

#include <string.h>

#include "asnix/arena.h"

enum { ART_LEAF = 0, ART_NODE4, ART_NODE16, ART_NODE48, ART_NODE256 };

typedef struct { uint8_t kind, prefix_len, prefix[3], count; } ArtHdr;

typedef struct { ArtHdr hdr; uint32_t key; } ArtLeaf;
typedef struct { ArtHdr hdr; uint8_t keys[4];   AsnixArtSymNode *children[4]; } ArtNode4;
typedef struct { ArtHdr hdr; uint8_t keys[16];  AsnixArtSymNode *children[16]; } ArtNode16;
typedef struct { ArtHdr hdr; uint8_t idx[256];  AsnixArtSymNode *children[48]; } ArtNode48;
typedef struct { ArtHdr hdr; AsnixArtSymNode *children[256]; } ArtNode256;

static inline uint8_t art_key_byte(uint32_t key, int depth) { return (uint8_t)(key >> (24 - 8 * depth)); }

static ArtLeaf *art_make_leaf(uint32_t key) {
    ArtLeaf *l = asnix_arena_alloc(sizeof(ArtLeaf));
    l->hdr.kind = ART_LEAF; l->hdr.prefix_len = 0; l->hdr.count = 0;
    l->key = key;
    return l;
}

static ArtNode4 *art_make_node4(uint8_t prefix_len, const uint8_t *prefix) {
    ArtNode4 *n = asnix_arena_alloc(sizeof(ArtNode4));
    n->hdr.kind = ART_NODE4; n->hdr.count = 0; n->hdr.prefix_len = prefix_len;
    for (int i = 0; i < prefix_len; i++) n->hdr.prefix[i] = prefix[i];
    return n;
}

/* Mutable insert: `ref` is the address of the pointer slot currently
 * holding this subtree's root (a field inside the parent node, or the
 * caller's own root). Every write either mutates the existing node in
 * place (room available) or allocates exactly one new node and
 * overwrites *ref -- no cloning of untouched ancestors. */
static void art_insert(AsnixArtSymNode **ref, uint32_t key, int depth) {
    AsnixArtSymNode *n = *ref;
    if (!n) { *ref = (AsnixArtSymNode *)art_make_leaf(key); return; }

    ArtHdr *h = (ArtHdr *)n;
    if (h->kind == ART_LEAF) {
        ArtLeaf *lf = (ArtLeaf *)n;
        if (lf->key == key) return; /* duplicate: no-op, no allocation */
        uint32_t other = lf->key;
        int d = depth, common = 0;
        while (common < 3 && art_key_byte(key, d + common) == art_key_byte(other, d + common)) common++;
        uint8_t pfx[3];
        for (int i = 0; i < common; i++) pfx[i] = art_key_byte(key, d + i);
        ArtNode4 *parent = art_make_node4((uint8_t)common, pfx);
        int bd = d + common;
        uint8_t b_old = art_key_byte(other, bd), b_new = art_key_byte(key, bd);
        parent->keys[0] = b_old; parent->children[0] = n; /* reuse existing leaf verbatim */
        parent->keys[1] = b_new; parent->children[1] = (AsnixArtSymNode *)art_make_leaf(key);
        parent->hdr.count = 2;
        *ref = (AsnixArtSymNode *)parent;
        return;
    }

    /* internal node: check/consume compressed prefix */
    int mismatch = -1;
    for (int i = 0; i < h->prefix_len; i++) if (h->prefix[i] != art_key_byte(key, depth + i)) { mismatch = i; break; }
    if (mismatch >= 0) {
        uint8_t pfx[3];
        for (int i = 0; i < mismatch; i++) pfx[i] = h->prefix[i];
        ArtNode4 *parent = art_make_node4((uint8_t)mismatch, pfx);
        uint8_t b_old = h->prefix[mismatch];
        uint8_t b_new = art_key_byte(key, depth + mismatch);
        /* truncate the existing node's prefix IN PLACE -- mutable, safe,
         * nothing else aliases it -- and reuse it as a child. */
        int newlen = h->prefix_len - mismatch - 1;
        for (int i = 0; i < newlen; i++) h->prefix[i] = h->prefix[mismatch + 1 + i];
        h->prefix_len = (uint8_t)newlen;
        parent->keys[0] = b_old; parent->children[0] = n;
        parent->keys[1] = b_new; parent->children[1] = (AsnixArtSymNode *)art_make_leaf(key);
        parent->hdr.count = 2;
        *ref = (AsnixArtSymNode *)parent;
        return;
    }
    depth += h->prefix_len;
    uint8_t byte = art_key_byte(key, depth);

    switch (h->kind) {
    case ART_NODE4: {
        ArtNode4 *n4 = (ArtNode4 *)n;
        for (int i = 0; i < h->count; i++) if (n4->keys[i] == byte) { art_insert(&n4->children[i], key, depth + 1); return; }
        if (h->count < 4) { n4->keys[h->count] = byte; n4->children[h->count] = (AsnixArtSymNode *)art_make_leaf(key); h->count++; return; }
        ArtNode16 *n16 = asnix_arena_alloc(sizeof(ArtNode16));
        n16->hdr.kind = ART_NODE16; n16->hdr.prefix_len = h->prefix_len;
        memcpy(n16->hdr.prefix, h->prefix, sizeof(n16->hdr.prefix));
        n16->hdr.count = h->count;
        for (int i = 0; i < h->count; i++) { n16->keys[i] = n4->keys[i]; n16->children[i] = n4->children[i]; }
        n16->keys[n16->hdr.count] = byte; n16->children[n16->hdr.count] = (AsnixArtSymNode *)art_make_leaf(key); n16->hdr.count++;
        *ref = (AsnixArtSymNode *)n16;
        return;
    }
    case ART_NODE16: {
        ArtNode16 *n16 = (ArtNode16 *)n;
        for (int i = 0; i < h->count; i++) if (n16->keys[i] == byte) { art_insert(&n16->children[i], key, depth + 1); return; }
        if (h->count < 16) { n16->keys[h->count] = byte; n16->children[h->count] = (AsnixArtSymNode *)art_make_leaf(key); h->count++; return; }
        ArtNode48 *n48 = asnix_arena_alloc(sizeof(ArtNode48));
        n48->hdr.kind = ART_NODE48; n48->hdr.prefix_len = h->prefix_len;
        memcpy(n48->hdr.prefix, h->prefix, sizeof(n48->hdr.prefix));
        n48->hdr.count = h->count;
        memset(n48->idx, 0, sizeof(n48->idx));
        for (int i = 0; i < h->count; i++) { n48->idx[n16->keys[i]] = (uint8_t)(i + 1); n48->children[i] = n16->children[i]; }
        n48->idx[byte] = (uint8_t)(n48->hdr.count + 1); n48->children[n48->hdr.count] = (AsnixArtSymNode *)art_make_leaf(key); n48->hdr.count++;
        *ref = (AsnixArtSymNode *)n48;
        return;
    }
    case ART_NODE48: {
        ArtNode48 *n48 = (ArtNode48 *)n;
        if (n48->idx[byte]) { art_insert(&n48->children[n48->idx[byte] - 1], key, depth + 1); return; }
        if (h->count < 48) { n48->idx[byte] = (uint8_t)(h->count + 1); n48->children[h->count] = (AsnixArtSymNode *)art_make_leaf(key); h->count++; return; }
        ArtNode256 *n256 = asnix_arena_alloc(sizeof(ArtNode256));
        n256->hdr.kind = ART_NODE256; n256->hdr.prefix_len = h->prefix_len;
        memcpy(n256->hdr.prefix, h->prefix, sizeof(n256->hdr.prefix));
        n256->hdr.count = h->count;
        memset(n256->children, 0, sizeof(n256->children));
        for (int b = 0; b < 256; b++) if (n48->idx[b]) n256->children[b] = n48->children[n48->idx[b] - 1];
        n256->children[byte] = (AsnixArtSymNode *)art_make_leaf(key);
        n256->hdr.count++;
        *ref = (AsnixArtSymNode *)n256;
        return;
    }
    default: { /* ART_NODE256 */
        ArtNode256 *n256 = (ArtNode256 *)n;
        if (n256->children[byte]) { art_insert(&n256->children[byte], key, depth + 1); return; }
        n256->children[byte] = (AsnixArtSymNode *)art_make_leaf(key);
        h->count++;
        return;
    }
    }
}

void asnix_art_sym_insert(AsnixArtSymSet *set, uint32_t sym) {
    art_insert(&set->root, sym, 0);
}

/* In-order traversal: Node4/16 store children in append order, not byte
 * order, so sort the small local (byte,child) list before recursing;
 * Node48/256 are already byte-indexed so iterating byte 0..255 is free
 * and already sorted (art_key_byte is MSB-first, so trie order == numeric
 * ascending key order). */
static void art_collect_sorted(const AsnixArtSymNode *n, uint32_t *out, int64_t *out_n) {
    if (!n) return;
    const ArtHdr *h = (const ArtHdr *)n;
    if (h->kind == ART_LEAF) { out[(*out_n)++] = ((const ArtLeaf *)n)->key; return; }
    if (h->kind == ART_NODE4 || h->kind == ART_NODE16) {
        int cnt = h->count;
        uint8_t keys[16]; const AsnixArtSymNode *children[16];
        if (h->kind == ART_NODE4) {
            const ArtNode4 *n4 = (const ArtNode4 *)n;
            for (int i = 0; i < cnt; i++) { keys[i] = n4->keys[i]; children[i] = n4->children[i]; }
        } else {
            const ArtNode16 *n16 = (const ArtNode16 *)n;
            for (int i = 0; i < cnt; i++) { keys[i] = n16->keys[i]; children[i] = n16->children[i]; }
        }
        for (int i = 1; i < cnt; i++) {
            int j = i; uint8_t k = keys[i]; const AsnixArtSymNode *c = children[i];
            while (j > 0 && keys[j - 1] > k) { keys[j] = keys[j - 1]; children[j] = children[j - 1]; j--; }
            keys[j] = k; children[j] = c;
        }
        for (int i = 0; i < cnt; i++) art_collect_sorted(children[i], out, out_n);
    } else if (h->kind == ART_NODE48) {
        const ArtNode48 *n48 = (const ArtNode48 *)n;
        for (int b = 0; b < 256; b++) if (n48->idx[b]) art_collect_sorted(n48->children[n48->idx[b] - 1], out, out_n);
    } else {
        const ArtNode256 *n256 = (const ArtNode256 *)n;
        for (int b = 0; b < 256; b++) if (n256->children[b]) art_collect_sorted(n256->children[b], out, out_n);
    }
}

int64_t asnix_art_sym_collect_sorted(const AsnixArtSymSet *set, uint32_t *out) {
    int64_t out_n = 0;
    art_collect_sorted(set->root, out, &out_n);
    return out_n;
}
