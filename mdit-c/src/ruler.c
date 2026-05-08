/*
 * ruler.c — ordered rule registry.
 *
 * Internal representation:
 *
 *   - rules[] — a vec of mdit_rule_record, indexed by insertion order.
 *     Mutators that change ordering (before/after/push) memmove the
 *     tail; given that rule counts top out near 30 even with plugins,
 *     the cost is negligible.
 *
 *   - cache  — a small map from chain-name to a vec of mdit_rule_entry.
 *     Marked dirty (cache.len = 0 + cache_dirty = true) on any
 *     mutation; lazily rebuilt on get_rules().
 */
#include "ruler.h"

#include <assert.h>
#include <string.h>

#include "vec.h"

/* ---------------------------------------------------------------------
 * Records
 * ------------------------------------------------------------------- */
typedef struct mdit_rule_record {
    mdit_str        name;
    bool            enabled;
    mdit_rule_fn    fn;
    void           *user;
    /* alt-chain tags. We copy the array into the ruler's arena so
     * callers don't have to keep their own array alive. The tag
     * strings themselves are still borrowed. */
    mdit_str       *alt;
    size_t          alt_len;
} mdit_rule_record;

MDIT_VEC_DECLARE(rule_records, mdit_rule_record)
MDIT_VEC_DEFINE (rule_records, mdit_rule_record)

MDIT_VEC_DECLARE(rule_entries, mdit_rule_entry)
MDIT_VEC_DEFINE (rule_entries, mdit_rule_entry)

MDIT_VEC_DECLARE(str_vec, mdit_str)
MDIT_VEC_DEFINE (str_vec, mdit_str)

/* ---------------------------------------------------------------------
 * Cache
 *
 * We store one mdit_vec_rule_entries per chain name. A simple
 * (chain_name, vec) array is fine — chain counts are tiny.
 * ------------------------------------------------------------------- */
typedef struct mdit_chain_cache {
    mdit_str               name;
    mdit_vec_rule_entries  entries;
} mdit_chain_cache;

MDIT_VEC_DECLARE(chain_cache, mdit_chain_cache)
MDIT_VEC_DEFINE (chain_cache, mdit_chain_cache)

/* ---------------------------------------------------------------------
 * Ruler
 * ------------------------------------------------------------------- */
struct mdit_ruler {
    mdit_arena             *arena;
    mdit_vec_rule_records   rules;

    bool                    cache_dirty;
    mdit_vec_chain_cache    cache;

    /* Scratch buffer for *rule_names() return values. Lives across
     * calls so callers get a stable pointer. */
    mdit_vec_str_vec        names_scratch;
};

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
mdit_ruler *mdit_ruler_new(mdit_arena *arena)
{
    mdit_ruler *r = (mdit_ruler *)mdit_arena_zalloc(arena, sizeof *r);
    r->arena       = arena;
    r->cache_dirty = true;
    mdit_vec_rule_records_init(&r->rules,         arena);
    mdit_vec_chain_cache_init (&r->cache,         arena);
    mdit_vec_str_vec_init     (&r->names_scratch, arena);
    return r;
}

void mdit_ruler_destroy(mdit_ruler *r)
{
    if (r == NULL) return;
    /* The arena owns everything; we just blank the bookkeeping. */
    mdit_vec_rule_records_destroy(&r->rules);
    mdit_vec_chain_cache_destroy (&r->cache);
    mdit_vec_str_vec_destroy     (&r->names_scratch);
    r->arena = NULL;
}

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */
static int find_rule(const mdit_ruler *r, mdit_str name)
{
    for (size_t i = 0; i < r->rules.len; ++i) {
        if (mdit_str_eq(r->rules.data[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static mdit_str *clone_alt(mdit_arena *a, const mdit_rule_options *opts)
{
    if (opts == NULL || opts->alt_len == 0) return NULL;
    mdit_str *copy = (mdit_str *)mdit_arena_alloc(
        a, opts->alt_len * sizeof(mdit_str));
    memcpy(copy, opts->alt, opts->alt_len * sizeof(mdit_str));
    return copy;
}

static void mark_dirty(mdit_ruler *r)
{
    r->cache_dirty = true;
    /* Reset cached vecs in place so the next get_rules rebuilds. */
    for (size_t i = 0; i < r->cache.len; ++i) {
        mdit_vec_rule_entries_clear(&r->cache.data[i].entries);
    }
}

static mdit_rule_status make_status(int idx, const char *msg)
{
    mdit_rule_status s; s.index = idx; s.message = msg; return s;
}

static mdit_rule_status insert_at(mdit_ruler *r, size_t pos,
                                  mdit_str name,
                                  mdit_rule_fn fn, void *user,
                                  mdit_rule_options opts)
{
    if (find_rule(r, name) != -1) {
        return make_status(MDIT_RULE_DUPLICATE, "rule name already registered");
    }
    /* Make room: emplace at end, then memmove the tail right by one. */
    if (mdit_vec_rule_records_emplace(&r->rules) == NULL) {
        return make_status(MDIT_RULE_OUT_OF_MEMORY, "ruler push: out of memory");
    }
    size_t len = r->rules.len;
    if (pos < len - 1) {
        memmove(&r->rules.data[pos + 1], &r->rules.data[pos],
                (len - 1 - pos) * sizeof(mdit_rule_record));
    }
    mdit_rule_record *slot = &r->rules.data[pos];
    slot->name    = name;
    slot->enabled = true;
    slot->fn      = fn;
    slot->user    = user;
    slot->alt     = clone_alt(r->arena, &opts);
    slot->alt_len = opts.alt_len;

    mark_dirty(r);
    return make_status((int)pos, NULL);
}

/* ---------------------------------------------------------------------
 * Public mutators
 * ------------------------------------------------------------------- */
mdit_rule_status mdit_ruler_push(mdit_ruler *r,
                                 mdit_str rule_name,
                                 mdit_rule_fn fn, void *user,
                                 mdit_rule_options opts)
{
    return insert_at(r, r->rules.len, rule_name, fn, user, opts);
}

mdit_rule_status mdit_ruler_before(mdit_ruler *r,
                                   mdit_str before_name,
                                   mdit_str rule_name,
                                   mdit_rule_fn fn, void *user,
                                   mdit_rule_options opts)
{
    int idx = find_rule(r, before_name);
    if (idx < 0) {
        return make_status(MDIT_RULE_NOT_FOUND, "anchor rule not found");
    }
    return insert_at(r, (size_t)idx, rule_name, fn, user, opts);
}

mdit_rule_status mdit_ruler_after(mdit_ruler *r,
                                  mdit_str after_name,
                                  mdit_str rule_name,
                                  mdit_rule_fn fn, void *user,
                                  mdit_rule_options opts)
{
    int idx = find_rule(r, after_name);
    if (idx < 0) {
        return make_status(MDIT_RULE_NOT_FOUND, "anchor rule not found");
    }
    return insert_at(r, (size_t)idx + 1, rule_name, fn, user, opts);
}

mdit_rule_status mdit_ruler_at(mdit_ruler *r,
                               mdit_str rule_name,
                               mdit_rule_fn fn, void *user,
                               mdit_rule_options opts)
{
    int idx = find_rule(r, rule_name);
    if (idx < 0) {
        return make_status(MDIT_RULE_NOT_FOUND, "rule not found");
    }
    mdit_rule_record *slot = &r->rules.data[idx];
    slot->fn      = fn;
    slot->user    = user;
    slot->alt     = clone_alt(r->arena, &opts);
    slot->alt_len = opts.alt_len;
    /* `at` does NOT touch enabled flag — matches upstream. */
    mark_dirty(r);
    return make_status(idx, NULL);
}

/* ---------------------------------------------------------------------
 * Enable / disable
 * ------------------------------------------------------------------- */
static int toggle(mdit_ruler *r, const mdit_str *names, size_t n,
                  bool target, bool ignore_invalid)
{
    int touched = 0;
    for (size_t i = 0; i < n; ++i) {
        int idx = find_rule(r, names[i]);
        if (idx < 0) {
            if (ignore_invalid) continue;
            return -1;
        }
        if (r->rules.data[idx].enabled != target) {
            r->rules.data[idx].enabled = target;
        }
        ++touched;
    }
    if (touched > 0) mark_dirty(r);
    return touched;
}

int mdit_ruler_enable(mdit_ruler *r,
                      const mdit_str *names, size_t nnames,
                      bool ignore_invalid)
{
    return toggle(r, names, nnames, true, ignore_invalid);
}

int mdit_ruler_disable(mdit_ruler *r,
                       const mdit_str *names, size_t nnames,
                       bool ignore_invalid)
{
    return toggle(r, names, nnames, false, ignore_invalid);
}

int mdit_ruler_enable_only(mdit_ruler *r,
                           const mdit_str *names, size_t nnames,
                           bool ignore_invalid)
{
    /* Disable everything, then re-enable the named subset. Matches
     * Python's enableOnly semantics including the "don't disable
     * unknowns first; raise on the enable step" ordering. */
    for (size_t i = 0; i < r->rules.len; ++i) {
        r->rules.data[i].enabled = false;
    }
    int n = toggle(r, names, nnames, true, ignore_invalid);
    mark_dirty(r);
    return n;
}

/* ---------------------------------------------------------------------
 * Cache + lookup
 * ------------------------------------------------------------------- */
static mdit_chain_cache *find_chain(mdit_ruler *r, mdit_str chain)
{
    for (size_t i = 0; i < r->cache.len; ++i) {
        if (mdit_str_eq(r->cache.data[i].name, chain)) {
            return &r->cache.data[i];
        }
    }
    return NULL;
}

static mdit_chain_cache *ensure_chain(mdit_ruler *r, mdit_str chain)
{
    mdit_chain_cache *existing = find_chain(r, chain);
    if (existing != NULL) return existing;

    mdit_chain_cache *slot = mdit_vec_chain_cache_emplace(&r->cache);
    slot->name = chain;
    mdit_vec_rule_entries_init(&slot->entries, r->arena);
    return slot;
}

static void rebuild_cache(mdit_ruler *r)
{
    /* Clear all chain entry vecs (we keep the chain-name slots but
     * the entry storage gets rewound). */
    for (size_t i = 0; i < r->cache.len; ++i) {
        mdit_vec_rule_entries_clear(&r->cache.data[i].entries);
    }
    /* Always have a default chain '' present. */
    (void)ensure_chain(r, MDIT_STR_LIT(""));

    /* First pass: discover any new alt-chains a rule belongs to and
     * make sure they exist in the cache. Then a second pass populates. */
    for (size_t i = 0; i < r->rules.len; ++i) {
        const mdit_rule_record *rec = &r->rules.data[i];
        if (!rec->enabled) continue;
        for (size_t j = 0; j < rec->alt_len; ++j) {
            (void)ensure_chain(r, rec->alt[j]);
        }
    }

    for (size_t c = 0; c < r->cache.len; ++c) {
        mdit_chain_cache *cc = &r->cache.data[c];
        bool is_default = (cc->name.len == 0);
        for (size_t i = 0; i < r->rules.len; ++i) {
            const mdit_rule_record *rec = &r->rules.data[i];
            if (!rec->enabled) continue;
            if (!is_default) {
                bool found = false;
                for (size_t j = 0; j < rec->alt_len; ++j) {
                    if (mdit_str_eq(rec->alt[j], cc->name)) {
                        found = true; break;
                    }
                }
                if (!found) continue;
            }
            mdit_rule_entry *e = mdit_vec_rule_entries_emplace(&cc->entries);
            e->fn   = rec->fn;
            e->user = rec->user;
        }
    }
    r->cache_dirty = false;
}

const mdit_rule_entry *mdit_ruler_get_rules(mdit_ruler *r,
                                            mdit_str chain,
                                            size_t *out_n)
{
    if (r->cache_dirty) rebuild_cache(r);
    mdit_chain_cache *cc = find_chain(r, chain);
    if (cc == NULL) {
        if (out_n) *out_n = 0;
        return NULL;
    }
    if (out_n) *out_n = cc->entries.len;
    return cc->entries.data;
}

const mdit_str *mdit_ruler_all_rule_names(mdit_ruler *r, size_t *out_n)
{
    mdit_vec_str_vec_clear(&r->names_scratch);
    for (size_t i = 0; i < r->rules.len; ++i) {
        (void)mdit_vec_str_vec_push(&r->names_scratch, r->rules.data[i].name);
    }
    if (out_n) *out_n = r->names_scratch.len;
    return r->names_scratch.data;
}

const mdit_str *mdit_ruler_active_rule_names(mdit_ruler *r, size_t *out_n)
{
    mdit_vec_str_vec_clear(&r->names_scratch);
    for (size_t i = 0; i < r->rules.len; ++i) {
        if (!r->rules.data[i].enabled) continue;
        (void)mdit_vec_str_vec_push(&r->names_scratch, r->rules.data[i].name);
    }
    if (out_n) *out_n = r->names_scratch.len;
    return r->names_scratch.data;
}

bool mdit_ruler_has(const mdit_ruler *r, mdit_str name)
{
    return find_rule(r, name) >= 0;
}

bool mdit_ruler_is_rule_enabled(const mdit_ruler *r, mdit_str name)
{
    int idx = find_rule(r, name);
    if (idx < 0) return false;
    return r->rules.data[idx].enabled;
}
