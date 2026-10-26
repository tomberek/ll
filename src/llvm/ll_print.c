/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/c/main.c's
 * print_string_contents, print_attr_name, print_active_push, print_active_pop,
 * print_attrs_seen_mark, print_peel_wrappers, print_value_inner,
 * print_value_concrete, print_value --
 * see ~/.claude/plans/soft-wishing-engelbart.md. Kept behaviorally identical
 * to that reference (escaping rules, attrset key quoting/sort order,
 * Cell/Thunk/LazyApp/ThunkMap ancestor-cycle tracking, <PRIMOP>/<LAMBDA>/
 * <PRIMOP-APP> states) -- see that file's own extensive commentary for the
 * full rationale behind each choice, not repeated here. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "builtins_shared.h"
#include "ll_print.h"
#include "symtab.h"
#include "value_util.h"

static void print_string_contents(const char *bytes, int64_t len) {
    for (int64_t i = 0; i < len; i++) {
        char c = bytes[i];
        switch (c) {
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '$':
                if (i + 1 < len && bytes[i + 1] == '{') {
                    fputs("\\${", stdout);
                    i++;
                } else {
                    putchar('$');
                }
                break;
            default: putchar(c); break;
        }
    }
}

static void print_string_bytes(const char *bytes, int64_t len) {
    putchar('"');
    print_string_contents(bytes, len);
    putchar('"');
}

static int attr_name_needs_quoting(const char *name, size_t len) {
    static const struct {
        const char *kw;
        size_t len;
    } RESERVED[] = {
        {"let", 3}, {"in", 2}, {"if", 2}, {"then", 4}, {"else", 4},
        {"rec", 3}, {"inherit", 7}, {"assert", 6}, {"with", 4}, {"or", 2},
    };
    if (len == 0) return 1;
    char c0 = name[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return 1;
    for (size_t i = 1; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '\'' ||
                 c == '-';
        if (!ok) return 1;
    }
    for (size_t k = 0; k < sizeof(RESERVED) / sizeof(RESERVED[0]); k++) {
        if (len == RESERVED[k].len && memcmp(name, RESERVED[k].kw, len) == 0) return 1;
    }
    return 0;
}

static void print_attr_name(const char *name, size_t len) {
    if (attr_name_needs_quoting(name, len)) {
        putchar('"');
        print_string_contents(name, (int64_t)len);
        putchar('"');
    } else {
        fwrite(name, 1, len, stdout);
    }
}

#define ASNIX_PRINT_MAX_DEPTH 4096
static const void *g_print_active[ASNIX_PRINT_MAX_DEPTH];
static int g_print_active_count = 0;

static int print_active_push(const void *base) {
    for (int i = 0; i < g_print_active_count; i++) {
        if (g_print_active[i] == base) return 0;
    }
    if (g_print_active_count < ASNIX_PRINT_MAX_DEPTH) g_print_active[g_print_active_count++] = base;
    return 1;
}

static void print_active_pop(void) {
    if (g_print_active_count > 0) g_print_active_count--;
}

static const void *g_attrs_seen[ASNIX_PRINT_MAX_DEPTH];
static int g_attrs_seen_count = 0;

static int print_attrs_seen_mark(const void *base) {
    for (int i = 0; i < g_attrs_seen_count; i++) {
        if (g_attrs_seen[i] == base) return 0;
    }
    if (g_attrs_seen_count < ASNIX_PRINT_MAX_DEPTH) g_attrs_seen[g_attrs_seen_count++] = base;
    return 1;
}

static void print_value_concrete(Value v);

static int print_peel_wrappers(Value *v_out, int *pushed_out) {
    Value v = *v_out;
    int pushed = 0;
    for (;;) {
        if (asnix_value_is_cell(v)) {
            Value *target = asnix_cell_target(v);
            if (!print_active_push((const void *)target)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = *target;
            continue;
        }
        if (asnix_tag_of(v) == ASNIX_TAG_THUNK) {
            const void *thunk_addr = (const void *)(v.raw & ~0x7ULL);
            if (!print_active_push(thunk_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_force_for_print(v);
            continue;
        }
        if (asnix_value_is_lazyapp(v)) {
            const void *lazyapp_addr = (const void *)(v.raw & ~0x7ULL);
            if (!print_active_push(lazyapp_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_resolve_lazyapp(v);
            continue;
        }
        if (asnix_value_is_thunkmap(v)) {
            const void *slot_addr = (const void *)asnix_thunkmap_slot(v);
            if (!print_active_push(slot_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_resolve_thunkmap(v);
            continue;
        }
        if (asnix_value_is_formalsmap(v)) { /* ll-only, see heap.h */
            /* NOT asnix_formalsmap_slot(v): that's the bare env slot this
             * descriptor memoizes into, which is the exact same address a
             * `try_cell_ref`-emitted Cell wrapping this same formal (e.g.
             * inside a list literal) already pushed one peel step up --
             * colliding there flagged even the very first resolution as a
             * false cycle (caught by a hand-built test printing the same
             * required formal multiple times in one list literal). v.raw
             * (tag bits set, idx packed into bits 48-63) is never bit-
             * identical to a bare 8-byte-aligned pointer, so it can't
             * collide with the Cell layer's own key, while still being
             * identical across genuinely repeated resolutions of this
             * same (descriptor, idx) pair. */
            const void *slot_addr = (const void *)(uintptr_t)v.raw;
            if (!print_active_push(slot_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_resolve_formalsmap(v);
            continue;
        }
        break;
    }
    *v_out = v;
    *pushed_out = pushed;
    return 1;
}

static void print_value_inner(Value v) {
    int pushed;
    if (!print_peel_wrappers(&v, &pushed)) return;
    print_value_concrete(v);
    for (int i = 0; i < pushed; i++) print_active_pop();
}

static void print_value_concrete(Value v) {
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT:
            printf("%lld", (long long)asnix_int_value(v));
            return;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: printf("true"); return;
                case ASNIX_IMM_FALSE: printf("false"); return;
                case ASNIX_IMM_NULL: printf("null"); return;
                default: printf("<imm %d>", asnix_imm_subtag(v)); return;
            }
        case ASNIX_TAG_PTR: {
            if (asnix_value_is_float(v)) {
                printf("%.6g", asnix_float_value(v));
                return;
            }
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            if (asnix_value_is_path(v)) printf("%.*s", (int)len, bytes);
            else print_string_bytes(bytes, len);
            return;
        }
        case ASNIX_TAG_LIST: {
            const char *base = (const char *)(v.raw & ~0x7ULL);
            int64_t count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
            printf("[ ");
            for (int64_t i = 0; i < count; i++) {
                print_value_inner(elems[i]);
                printf(" ");
            }
            printf("]");
            return;
        }
        case ASNIX_TAG_ATTRS: {
            v = asnix_bshared_attrs_resolve(v);
            const char *base = (const char *)(v.raw & ~0x7ULL);
            if (!print_attrs_seen_mark(base)) {
                printf("«repeated»");
                return;
            }
            int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
            const char *names[64];
            size_t name_lens[64];
            Value vals[64];
            int64_t n = count > 64 ? 64 : count;
            for (int64_t i = 0; i < n; i++) {
                const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
                uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
                names[i] = asnix_sym_name(sym, &name_lens[i]);
                vals[i] = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            }
            for (int64_t i = 1; i < n; i++) {
                int64_t j = i;
                while (j > 0) {
                    size_t minlen = name_lens[j - 1] < name_lens[j] ? name_lens[j - 1] : name_lens[j];
                    int cmp = memcmp(names[j - 1], names[j], minlen);
                    if (cmp == 0) cmp = (int)name_lens[j - 1] - (int)name_lens[j];
                    if (cmp <= 0) break;
                    const char *tn = names[j - 1];
                    names[j - 1] = names[j];
                    names[j] = tn;
                    size_t tl = name_lens[j - 1];
                    name_lens[j - 1] = name_lens[j];
                    name_lens[j] = tl;
                    Value tv = vals[j - 1];
                    vals[j - 1] = vals[j];
                    vals[j] = tv;
                    j--;
                }
            }
            printf("{ ");
            for (int64_t i = 0; i < n; i++) {
                print_attr_name(names[i], name_lens[i]);
                printf(" = ");
                print_value_inner(vals[i]);
                printf("; ");
            }
            printf("}");
            return;
        }
        case ASNIX_TAG_PRIMOP:
            printf("<PRIMOP>");
            return;
        case ASNIX_TAG_CLOSURE: {
            const char *base = (const char *)(v.raw & ~0x7ULL);
            Value formals = *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
            if (formals.raw == asnix_bshared_native_formals_marker().raw) {
                void *up = *(void **)(base + ASNIX_CLOSURE_OFF_UP);
                printf(up == NULL ? "<PRIMOP>" : "<PRIMOP-APP>");
            } else {
                printf("<LAMBDA>");
            }
            return;
        }
        default:
            printf("<unprintable value, tag=%d>", asnix_tag_of(v));
            return;
    }
}

void ll_print_value(Value v) {
    g_attrs_seen_count = 0;
    print_value_inner(v);
    printf("\n");
}
