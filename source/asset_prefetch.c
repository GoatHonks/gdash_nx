/* asset_prefetch.c -- warm the asset cache from a background thread.
 *
 * With icons/ split into buckets an icon open costs ~15 ms, but there is a
 * fixed floor of roughly 8-10 ms per open on this filesystem that no amount of
 * bucketing removes. The menu pulls in a new icon every 2-3 seconds and pays
 * two of those opens on the render thread, which is the stutter that survives
 * every other fix here.
 *
 * The only way past a per-open floor is to have already done the open. This
 * walks the indexed tree on a background core and pulls files into the cache
 * ahead of demand, so the render thread finds them already in memory.
 *
 * SD access is serialised, so an unthrottled warmer would compete with the
 * render thread's own opens and make individual stalls worse before making
 * them disappear. Hence: lowest thread priority, a background core, a sleep
 * between every file, and a delayed start so the game's own load finishes
 * first. It gives up as soon as the cache stops accepting entries.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "asset_prefetch.h"
#include "asset_index.h"
#include "asset_cache.h"
#include "paths.h"
#include "pthr.h"

#define STACK_SIZE     (64 * 1024)
#define PRIO_LOWEST    0x3B   // well below the render thread; yields readily
#define START_DELAY_NS 3000000000ull // 3 s: let the game's own loading settle
// A FIXED gap only works if reads are fast. On a folder where each read costs
// 100-300 ms (fragmented card, or simply a slower corner of it) a fixed 3 ms
// sleep means this thread owns the card almost continuously: the render
// thread's own opens then queue behind it and inflate to match, which is worse
// than never prefetching at all. Measured on Meltdown: opens went from 19 ms to
// 185 ms once this thread started, and a 3-file directory cost 149 ms.
//
// So the gap is now proportional to the work just done -- sleep 3x the time the
// read took, i.e. use at most ~25% of the device -- and if reads stay slow the
// thread gives up entirely, because on such a card prefetching cannot win.
#define DUTY_MULTIPLIER 3ull          // sleep 3x the read time => ~25% duty
#define MIN_GAP_NS      2000000ull    // 2 ms
#define MAX_GAP_NS      500000000ull  // 0.5 s
#define SLOW_READ_NS    25000000ull   // 25 ms average => this card is too slow
#define SLOW_WINDOW     64u           // files per assessment

static Thread s_thread;
static volatile int s_stop;
static volatile int s_running;
static volatile unsigned s_done, s_skipped;
static volatile int s_finished;
static volatile int s_gave_up;      // reads too slow: backed off to protect the frame
static volatile unsigned s_avg_ms;  // rolling average read cost, for the log
static u64 s_win_ns;
static unsigned s_win_n;

// One pass over the index. `want_icons` selects the icons/ subtree first,
// since that is where demand actually falls; a second pass takes the rest.
static void pass(int want_icons) {
  const unsigned slots = asset_index_slots();
  for (unsigned i = 0; i < slots && !s_stop; i++) {
    const char *rel = asset_index_at(i);
    if (!rel)
      continue;
    const int is_icon = (strncasecmp(rel, "icons/", 6) == 0);
    if (is_icon != want_icons)
      continue;
    if (!strrchr(rel, '.'))
      continue; // no extension: a directory, not something to read

    char abs[768];
    snprintf(abs, sizeof(abs), "%s/%s", path_assets(), rel);

    const u64 t0 = armGetSystemTick();
    FILE *f = asset_cache_open(abs);
    if (f) {
      fclose(f); // the bytes stay in the cache; we only wanted them resident
      s_done++;
    } else {
      s_skipped++; // audio, over budget, or unreadable -- all fine to skip
    }
    const u64 cost = armTicksToNs(armGetSystemTick() - t0);

    // Give up if this device is simply slow. Prefetching only helps when a
    // read is cheap; when it is not, the contention costs more than the cache
    // saves, and the frame is what matters.
    s_win_ns += cost;
    if (++s_win_n >= SLOW_WINDOW) {
      const u64 avg = s_win_ns / s_win_n;
      s_avg_ms = (unsigned)(avg / 1000000ull);
      s_win_ns = 0;
      s_win_n = 0;
      if (avg > SLOW_READ_NS) {
        s_gave_up = 1;
        s_stop = 1;
        return;
      }
    }

    u64 gap = cost * DUTY_MULTIPLIER;
    if (gap < MIN_GAP_NS) gap = MIN_GAP_NS;
    if (gap > MAX_GAP_NS) gap = MAX_GAP_NS;
    svcSleepThread(gap);
  }
}

static void prefetch_main(void *arg) {
  (void)arg;
  pthr_pin_worker_core(); // never share the render thread's core
  svcSetThreadPriority(CUR_THREAD_HANDLE, PRIO_LOWEST);
  svcSleepThread(START_DELAY_NS);

  pass(1); // icons first: that is what the menu keeps asking for
  pass(0);

  s_finished = 1;
}

void asset_prefetch_start(void) {
  if (s_running)
    return;
  if (R_FAILED(threadCreate(&s_thread, prefetch_main, NULL, NULL, STACK_SIZE,
                            PRIO_LOWEST, -2)))
    return; // -2: let the kernel pick a core; we re-pin inside the thread
  if (R_FAILED(threadStart(&s_thread))) {
    threadClose(&s_thread);
    return;
  }
  s_running = 1;
}

void asset_prefetch_stop(void) {
  if (!s_running)
    return;
  s_stop = 1;
  threadWaitForExit(&s_thread);
  threadClose(&s_thread);
  s_running = 0;
}

void asset_prefetch_stats(unsigned *done, unsigned *skipped, int *finished) {
  if (done)     *done = s_done;
  if (skipped)  *skipped = s_skipped;
  if (finished) *finished = s_finished;
}

int asset_prefetch_gave_up(void) { return s_gave_up; }

unsigned asset_prefetch_avg_ms(void) { return s_avg_ms; }
