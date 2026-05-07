/*
 * utf.c — non-generated bits of the Unicode classification layer.
 *
 * ``mdit_is_punct`` lives in the generated utf_tables.c. We keep
 * ``mdit_is_whitespace`` here because the set is hand-rolled (matches
 * ``markdown_it.common.utils.isWhiteSpace``) and small enough to inline
 * as a couple of branches, no table needed.
 */
#include "utf.h"

bool mdit_is_whitespace(uint32_t cp)
{
    /* Hottest case first: ASCII space + tab. */
    if (cp == 0x20u) return true;
    if (cp >= 0x09u && cp <= 0x0Du) return true;        /* \t \n \v \f \r */

    /* Fast reject for the giant non-whitespace BMP region. */
    if (cp < 0xA0u) return false;

    if (cp == 0xA0u)   return true;        /* NBSP */
    if (cp == 0x1680u) return true;        /* OGHAM SPACE MARK */
    if (cp >= 0x2000u && cp <= 0x200Au) return true; /* en quad..hair space */
    if (cp == 0x202Fu) return true;        /* NARROW NO-BREAK SPACE */
    if (cp == 0x205Fu) return true;        /* MEDIUM MATHEMATICAL SPACE */
    if (cp == 0x3000u) return true;        /* IDEOGRAPHIC SPACE */
    return false;
}
