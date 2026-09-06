/* asset_prefetch.h -- see asset_prefetch.c
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ASSET_PREFETCH_H__
#define __ASSET_PREFETCH_H__

// Start/stop the background cache warmer. Safe to call stop without start.
void asset_prefetch_start(void);
void asset_prefetch_stop(void);

void asset_prefetch_stats(unsigned *done, unsigned *skipped, int *finished);

// 1 once the thread backed off because reads were too slow to be worth it
int asset_prefetch_gave_up(void);
unsigned asset_prefetch_avg_ms(void);

#endif
