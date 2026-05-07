/*
 * entities.h — HTML5 named entity lookup.
 *
 * Backed by a sorted, binary-searched table generated from Python's
 * ``html.entities.html5`` (see scripts/gen_entities.py and
 * src/entities_table.c). The table contains the same ~2125 names that
 * ``markdown_it.common.entities`` exposes, with trailing semicolons
 * stripped to match the Python view.
 *
 * Lookup is case-sensitive, matching upstream behaviour. Callers must
 * normalize / split the entity name themselves before calling here
 * (i.e. given ``&AMP;`` in source text, the inline rule passes
 * ``"AMP"`` of length 3 to this lookup).
 */
#ifndef MDIT_SRC_ENTITIES_H
#define MDIT_SRC_ENTITIES_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Look up an entity by name. On success, ``*out`` points into a static
 * read-only buffer with ``*out_len`` bytes of UTF-8. The returned
 * pointer is *not* NUL-terminated (some entities expand to a multi-
 * codepoint string with embedded NUL-safe content).
 *
 * Returns true on a hit, false otherwise. The out parameters may be
 * NULL if the caller only wants to test for presence.
 */
bool   mdit_entity_lookup(const char *name, size_t len,
                          const char **out, size_t *out_len);

/*
 * Number of entries in the table. Useful for tests; not part of the
 * parser hot path.
 */
size_t mdit_entity_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_ENTITIES_H */
