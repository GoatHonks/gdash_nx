/* asset_cache.h -- see asset_cache.c
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ASSET_CACHE_H__
#define __ASSET_CACHE_H__

#include <stdio.h>
#include <stddef.h>

void asset_cache_init(const char *root, size_t budget);

// A readable stream over the cached bytes, or NULL if this path is not
// cacheable / could not be cached. NULL means "open it normally".
FILE *asset_cache_open(const char *path);

void asset_cache_stats(unsigned *files, unsigned *hits, unsigned *misses,
                       size_t *bytes);

#endif
