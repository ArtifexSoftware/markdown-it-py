/*
 * linkifier_tlds_full.h — full ICANN TLD list (opt-in).
 *
 * The implementation file (`linkifier_tlds_full.c`) is auto-generated
 * by `mdit-c/scripts/gen_tlds.py` from `linkify-it-py.tlds.TLDS`.
 *
 * Consumers should not call this lookup directly; the default
 * linkifier consults it via `mdit_linkifier_default_use_full_tlds()`.
 */
#ifndef MDIT_SRC_LINKIFIER_TLDS_FULL_H
#define MDIT_SRC_LINKIFIER_TLDS_FULL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if `s[0..n)` is a known TLD in the full ICANN list.
 * Comparison is ASCII case-insensitive. */
bool   mdit_linkifier_full_tld_lookup(const char *s, size_t n);

/* Number of entries in the full TLD list. Useful for tests. */
size_t mdit_linkifier_full_tld_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_LINKIFIER_TLDS_FULL_H */
