/* asset_index.h -- see asset_index.c
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ASSET_INDEX_H__
#define __ASSET_INDEX_H__

// Walk the assets tree once and remember every path in it.
void asset_index_build(const char *root);

// 1 only when the index is built, the path is inside the tree, and it is not
// there. 0 means "exists, or we cannot say" -- callers must fall through to
// the real filesystem on 0.
int asset_index_missing(const char *path);

// Resolve a path whose file has been moved into a subdirectory; NULL if the
// path is already correct or genuinely unknown.
const char *asset_index_resolve(const char *path);

unsigned asset_index_aliases(void);

// Iterate every indexed path: slot i holds a relative path, or NULL.
unsigned asset_index_slots(void);
const char *asset_index_at(unsigned i);

unsigned asset_index_count(void);

// 0 once the index has caught itself being wrong and switched off
int asset_index_active(void);

// how long the walk took, and how many stat() calls it needed
unsigned asset_index_build_ms(void);
unsigned asset_index_stats(void);

#endif
