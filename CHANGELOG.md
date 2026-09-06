# Changelog

All changes in this fork, relative to upstream `NaGaa95/gdash_nx`.

Every change below is confined to the wrapper. No game code, game binaries or
game assets are modified or redistributed — you still supply your own.

---

## r2 — the other three games

Meltdown, SubZero and World stuttered even with everything above applied. Two
separate causes, which multiplied each other.

**The icon split must not be run on the card.** `split_icons.py` moves files
with a rename, which on the card's filesystem — FAT32 or exFAT, both behave the
same way — relocates each file's directory entry but never rewrites its data. Doing that to 4800 files leaves the new bucket directories
scattered across the card with every file's data still where it originally sat.
Reaching a 2.9 KB icon then cost more than reading an 87 KB file from the root
directory — 143 ms against 14 ms, the opposite of what size or file count would
predict. Copying the split tree onto the card fresh writes directories and data
down together:

| | split in place | copied fresh |
|---|---|---|
| index build | 1624 ms | **156 ms** |
| prefetch average read | 149 ms | **7 ms** |
| icon open | 143 ms | **7.7 ms** |

The script now refuses to let anyone repeat this quietly: the warning is in its
docstring and printed after every split.

**The prefetcher's throttle was wrong.** It slept a fixed 3 ms between files,
tuned on a folder where reads cost ~15 ms. On a folder where a read costs
150 ms, a 3 ms gap means the thread owns the card continuously — the render
thread's own opens then queue behind it and inflate to match. Measured: opens
went from 19 ms to 185 ms the moment it started, and a *three-file* directory
cost 149 ms.

The gap is now proportional to the work just done — sleep 3x the read time, so
it uses at most ~25% of the device however slow that device is — and if the
rolling average read stays above 25 ms it stops entirely, because prefetching
cannot pay for the contention it causes on such a card. That alone took
in-level play from constant stalls to 1.9/min at 28 ms while still fragmented.

**Also:** the asset cache budget was 96 MB, sized to Geometry Dash's 89 MB of
assets. The other three need 94-95 MB, which is not a margin. It is now 256 MB,
and the cache table went from 8192 to 16384 slots (load factor 0.71 to 0.35).

All four games now measure the same: index build 175-185 ms, average read
7-8 ms, and the prefetcher completing normally rather than giving up.

---

## Performance: the periodic stutter

The port stuttered every few seconds, badly enough to lose a run. It turned out
to be four separate causes, found by instrumenting the frame loop and measuring
rather than guessing. Idle wall-clock time lost to stalls went from **11.6% to
about 1%**, and render-thread asset opens went to **zero** once warm.

**Asset index** (`source/asset_index.c`)
Horizon resolves a path by scanning the containing directory and caches nothing
between calls, so opening a file costs more the more files sit beside it. The
port now walks the assets tree once at startup and answers "does this exist?"
from RAM, so a miss no longer touches the filesystem. The index is only trusted
if the walk actually succeeded, and it audits a sample of its own answers
against the real filesystem — if it is ever wrong it disables itself rather
than hiding a file that exists.

**Asset cache** (`source/asset_cache.c`)
Measured on hardware: 91% of the game's asset opens were files it had already
opened, because the menu re-reads the same icon sprites continuously. Those
bytes are now kept in RAM (89 MB covers every non-audio asset) and served with
`fmemopen`. Audio is deliberately excluded — FMOD streams it and needs a real
file handle.

**Icon directory bucketing** (`extras/split_icons.py` + index aliasing)
`assets/icons/` holds ~4800 files, and an open there measured **97 ms** against
**13 ms** in the 1025-file `assets/` root — the cost scales with directory size.
The script splits `icons/` into 16 buckets, and the port resolves the moved
files transparently, so the game is unaware.

This is the single largest part of the stutter fix. Measured across builds:
the RAM cache alone took idle stall time from 11.6% to 8.2%, and splitting
`icons/` took it from 8.2% to 1.5% — nearly twice the improvement. It also
cuts the prefetcher's warm-up from about 8 minutes to 75 seconds. An unsplit
install still works, because the index resolves both layouts, but it keeps
most of the stutter.

**Background prefetcher** (`source/asset_prefetch.c`)
Even bucketed, there is a fixed ~10 ms floor per open that no amount of
bucketing removes. A low-priority thread on a background core pulls assets into
the cache ahead of demand, throttled with a sleep between files so it never
competes with the render thread for the card.

## Autosave freeze — fixes issue #8

The ~20-second lag spikes were the autosave: `GameManager::doQuickSave()` was
called on the render thread every 1200 frames, serialising ~1 MB of state and
freezing the frame for **~253 ms**. Mid-level, that loses the run.

It now only runs when the player is genuinely idle, and on a BACK press, which
is how you pause or leave a level — the game is not in motion then, so the cost
is invisible. Saves on focus loss (HOME) and quit are unchanged.

## Controls — fixes issue #7

Pressing A in the custom level menu always hit the door in the bottom-right
corner, because the port synthesised a touch at a **fixed screen coordinate**
for jump. It cannot know whether you are in a level or a menu, so a fixed point
is wrong half the time.

Clicks are now anchored to the on-screen cursor. In a level any tap jumps, so
position is irrelevant there; in a menu it clicks exactly what you aimed at.
With the cursor hidden it taps a configurable spot, aimed by default at the
level box on the level-select screen.

The left/right buttons were aimed at coordinates inherited from the Vita port's
*platformer* arrows, which are nowhere near this build's level-select arrows —
so they hit empty space. They now default to the real arrow positions, measured
from a 1280x720 capture.

## Configurable controls

`config.txt` gained button and position options, and is rewritten each launch so
new keys appear instead of silently using invisible defaults:

- `click_buttons`, `left_buttons`, `right_buttons`, `back_buttons` — comma
  separated, or `none` to disable. A typo falls back to the default rather than
  leaving you without a control.
- `click_zone_x/y`, `left_zone_x/y`, `right_zone_x/y` — tap positions in percent
  of the screen, so they hold in both handheld and docked.
- `show_zones 1` — draws a marker at each tap position so they can be aimed.

## Smaller fixes

- Save files are opened with a large buffer. They were falling back to newlib's
  1 KB `BUFSIZ`, turning a 1 MB save into thousands of blocking filesystem
  calls.
- `rename()` tries the rename before removing the destination, saving a
  filesystem round trip per save.
- `.gitignore` now excludes game binaries and assets, so they cannot be
  committed by accident.
