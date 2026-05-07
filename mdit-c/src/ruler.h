/*
 * ruler.h — ordered rule registry, mirrors ``markdown_it.ruler.Ruler``.
 *
 * The ruler stores a list of named rules (function pointer + opaque
 * user data + ``alt`` chain tags) and maintains a per-chain cache of
 * the active subset. The C port mirrors the upstream API 1:1 so
 * porting plugins is mechanical:
 *
 *   - ``before`` / ``after`` / ``at`` / ``push`` install rules,
 *   - ``enable`` / ``disable`` / ``enable_only`` flip the bit,
 *   - ``get_rules(chain)`` returns the cached active subset,
 *   - ``get_all_rules`` / ``get_active_rules`` enumerate names.
 *
 * Lifetime:
 *   - All ruler-internal allocations come from a borrowed arena.
 *   - Rule names and ``alt`` tag strings are *borrowed*; callers must
 *     keep them alive for the ruler's lifetime (the parser holds them
 *     as static literals, so this is trivial).
 *   - Cache invalidation is automatic: any mutator marks the cache
 *     dirty; ``get_rules`` lazy-rebuilds.
 */
#ifndef MDIT_SRC_RULER_H
#define MDIT_SRC_RULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generic rule function pointer; the parser cores cast to specific
 * signatures (``state_block *``, ``state_inline *`` etc) at the call
 * site. We keep the generic signature here to avoid pulling state
 * headers into the ruler. */
typedef bool (*mdit_rule_fn)(void *state, void *user);

typedef struct mdit_rule_options {
    const mdit_str *alt;     /* borrowed array of alt-chain tags */
    size_t          alt_len;
} mdit_rule_options;

#define MDIT_RULE_OPTIONS_NONE ((mdit_rule_options){ NULL, 0 })

typedef struct mdit_rule_status {
    int            index;     /* >=0 on success, negative on error */
    const char    *message;   /* static literal on error, else NULL */
} mdit_rule_status;

/* Status codes returned via mdit_rule_status.index when negative. */
enum {
    MDIT_RULE_OK            = 0,
    MDIT_RULE_NOT_FOUND     = -1,
    MDIT_RULE_OUT_OF_MEMORY = -2,
    MDIT_RULE_DUPLICATE     = -3
};

/* Opaque types — internals live in ruler.c. */
typedef struct mdit_ruler  mdit_ruler;

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
mdit_ruler *mdit_ruler_new   (mdit_arena *arena);
void        mdit_ruler_destroy(mdit_ruler *r);  /* idempotent */

/* ---------------------------------------------------------------------
 * Mutation
 *
 * Each insertion takes a borrowed name (mdit_str view), the function
 * pointer, an opaque user pointer (forwarded to the rule on each
 * call), and rule options.
 *
 * On any error the returned status carries a non-zero index AND a
 * message; otherwise index is the position the rule landed at.
 * ------------------------------------------------------------------- */
mdit_rule_status mdit_ruler_push  (mdit_ruler *r,
                                   mdit_str rule_name,
                                   mdit_rule_fn fn, void *user,
                                   mdit_rule_options opts);
mdit_rule_status mdit_ruler_before(mdit_ruler *r,
                                   mdit_str before_name,
                                   mdit_str rule_name,
                                   mdit_rule_fn fn, void *user,
                                   mdit_rule_options opts);
mdit_rule_status mdit_ruler_after (mdit_ruler *r,
                                   mdit_str after_name,
                                   mdit_str rule_name,
                                   mdit_rule_fn fn, void *user,
                                   mdit_rule_options opts);
mdit_rule_status mdit_ruler_at    (mdit_ruler *r,
                                   mdit_str rule_name,
                                   mdit_rule_fn fn, void *user,
                                   mdit_rule_options opts);

/* ---------------------------------------------------------------------
 * Enable / disable. ``ignore_invalid`` controls whether unknown names
 * raise (as MDIT_RULE_NOT_FOUND) or are silently skipped.
 *
 * The functions return the number of rules they actually toggled
 * (non-negative); on a hard error (-1 etc.) nothing was toggled.
 * ------------------------------------------------------------------- */
int  mdit_ruler_enable     (mdit_ruler *r,
                            const mdit_str *names, size_t nnames,
                            bool ignore_invalid);
int  mdit_ruler_enable_only(mdit_ruler *r,
                            const mdit_str *names, size_t nnames,
                            bool ignore_invalid);
int  mdit_ruler_disable    (mdit_ruler *r,
                            const mdit_str *names, size_t nnames,
                            bool ignore_invalid);

/* ---------------------------------------------------------------------
 * Lookup / iteration
 *
 * ``get_rules`` returns a contiguous array of (fn, user) pairs for the
 * active rules in the requested chain. Pass ``MDIT_STR_LIT("")`` to
 * get the default chain.
 * ------------------------------------------------------------------- */
typedef struct mdit_rule_entry {
    mdit_rule_fn  fn;
    void         *user;
} mdit_rule_entry;

const mdit_rule_entry *mdit_ruler_get_rules(mdit_ruler *r,
                                            mdit_str chain,
                                            size_t *out_n);

/* Snapshot of all (regardless of enabled state) or only enabled rule
 * names; the returned array is owned by the caller's arena and remains
 * valid until the next mutator call on the ruler. */
const mdit_str *mdit_ruler_all_rule_names   (mdit_ruler *r, size_t *out_n);
const mdit_str *mdit_ruler_active_rule_names(mdit_ruler *r, size_t *out_n);

/* True iff a rule with that name exists. */
bool mdit_ruler_has(const mdit_ruler *r, mdit_str name);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_RULER_H */
