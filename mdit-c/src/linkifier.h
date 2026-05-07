/*
 * linkifier.h — built-in URL/email scanner used by the linkify rules.
 *
 * This is a partial port of `linkify-it-py`. The default linkifier
 * recognizes:
 *
 *   - explicit-scheme URLs (`http://`, `https://`, `ftp://`)
 *   - protocol-relative URLs (`//host/...`) when preceded by an
 *     allowed character
 *   - explicit `mailto:` URLs
 *   - plain email addresses `local@host.tld` (TLD-validated)
 *   - bare hostnames `www.host.tld/...` and `host.tld/...` against
 *     a small built-in TLD list
 *
 * The default TLD set matches upstream linkify-it's *default* config:
 *
 *   - the small built-in list (biz, com, edu, gov, net, org, pro, web,
 *     xxx, aero, asia, coop, info, museum, name, shop, рф)
 *   - all 2-character ASCII ccTLDs
 *   - any Punycode label `xn--<1..59 ASCII alnum/hyphens>` (IDN)
 *
 * Callers wanting recognition of *every* ICANN-delegated TLD (~1500
 * entries, including brand TLDs like `.google`, `.app`, …) can opt
 * in process-wide via :c:func:`mdit_linkifier_default_use_full_tlds`.
 * This mirrors upstream's `Linkify.tlds(TLDS, keep_old=True)`.
 *
 * What's intentionally NOT (yet) supported relative to upstream:
 *
 *   - IP-literal hosts inside fuzzy matches (`fuzzy_ip` is always off)
 *   - Custom user-registered schemas
 *
 * Usage:
 *   const mdit_linkifier *L = mdit_linkifier_default();
 *   mdit_md_set_linkifier(md, L);
 *   md->options.linkify = true;
 *   mdit_linkifier_default_use_full_tlds(true);  // optional
 */
#ifndef MDIT_SRC_LINKIFIER_H
#define MDIT_SRC_LINKIFIER_H

#include <stdbool.h>

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the singleton default linkifier. The pointer is stable for
 * the lifetime of the process; callers must not free it. */
const mdit_linkifier *mdit_linkifier_default(void);

/* Toggle inclusion of the full ICANN TLD list in the default
 * linkifier. The flag is process-wide and affects every parser using
 * the default singleton. Returns the previous value. The default is
 * `false` (matching upstream's default `Linkify()` constructor).
 *
 * Effect on the matcher:
 *
 *   - `true`  - any ICANN-delegated TLD is accepted as a fuzzy host's
 *               final label (e.g. `example.app`, `foo.museum`).
 *   - `false` - only the small built-in default list, 2-char ccTLDs,
 *               and `xn--*` Punycode labels are accepted.
 *
 * Note: explicit-scheme URLs (`http://...`, `mailto:...`) do not
 * consult the TLD list — they accept any host shape. The flag only
 * affects fuzzy / bare-host detection.
 */
bool mdit_linkifier_default_use_full_tlds(bool enable);

/* Query the current value of the toggle without changing it. */
bool mdit_linkifier_default_full_tlds_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_LINKIFIER_H */
