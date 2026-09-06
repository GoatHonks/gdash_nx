# Changelog

All changes in this fork, relative to upstream `NaGaa95/gdash_nx`.

Every change below is confined to the wrapper. No game code, game binaries or
game assets are modified or redistributed — you still supply your own.

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
files transparently, so the game is unaware. This is optional: an unsplit
install still works, just slower.

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
