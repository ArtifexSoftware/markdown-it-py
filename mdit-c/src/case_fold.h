/*
 * case_fold.h — Unicode case-fold lookup used by reference-label
 * normalization.
 *
 * The single function here mirrors Python's per-codepoint
 * ``chr(cp).lower().upper()`` round-trip, which is the operation
 * upstream ``markdown-it-py`` applies to reference labels (see
 * ``markdown_it.common.utils.normalizeReference``). The table itself
 * lives in the generated ``case_fold.c`` (see scripts/gen_casefold.py
 * for the rationale and the cross-version sanity check).
 *
 * The lookup is *not* a general-purpose Unicode case-folding function;
 * it skips Final_Sigma context handling because both ``σ`` and ``ς``
 * uppercase to ``Σ``, which is enough to make the round-trip identity
 * collapse all sigma variants. Callers that need full Unicode
 * casefolding (Caseless Matching, NFC, …) should not use this helper.
 */
#ifndef MDIT_SRC_CASE_FOLD_H
#define MDIT_SRC_CASE_FOLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Look up the ``lower().upper()`` mapping for ``cp``.
 *
 *   - Returns ``true`` if ``cp`` has a non-identity mapping. ``*out``
 *     is set to a pointer into a static UTF-8 byte pool (read-only,
 *     not NUL-terminated) and ``*len`` to its byte length (1..6).
 *
 *   - Returns ``false`` if ``cp`` maps to itself; ``*out`` and
 *     ``*len`` are unchanged. Callers should re-emit ``cp`` verbatim.
 *
 * Codepoints outside ``[0, 0x110000)`` always return ``false``.
 */
bool mdit_case_fold_lookup(uint32_t cp, const char **out, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_CASE_FOLD_H */
