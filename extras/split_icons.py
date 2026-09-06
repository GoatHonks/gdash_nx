#!/usr/bin/env python3
"""Split a large flat asset directory into buckets.

Horizon resolves a path by scanning the containing directory, and it caches
nothing between calls, so the cost of opening a file grows with the number of
entries beside it. Measured on a Switch Lite:

    assets/       1025 files    ~13 ms per open
    assets/icons/ 4828 files    ~97 ms per open

Geometry Dash opens two icon files (.png + .plist) whenever the menu shows a
new icon, so a 4800-entry icons/ directory costs ~200 ms of stall every couple
of seconds. Splitting it into 16 buckets of ~300 cuts the scan proportionally.

The port resolves the moved files automatically: asset_index builds an alias
from "icons/foo.png" to wherever foo.png actually lives, so the game is
unaware. Bucket names are arbitrary -- the port reads the real layout at
startup rather than recomputing this hash.

IMPORTANT -- do not run this directly on the SD card.

This script moves files with a rename. On the card's filesystem -- FAT32 or
exFAT, both behave this way -- that relocates each file's directory ENTRY but
never rewrites its data. So 4800 renames leave the new bucket directories
scattered across the card with every file's data still wherever it originally
landed. Measured on hardware (FAT32), that is worse than not splitting at all:

                        split in place    copied fresh
    index build         1624 ms            156 ms
    icon open            143 ms            7.7 ms

Instead: copy the game folder to a PC, run this on the copy, then delete
assets/icons/ on the card and copy the split folder back. Writing the tree fresh
lays the directories and the file data down together.

Usage:
    python3 split_icons.py <path-to-assets/icons>          # split
    python3 split_icons.py <path-to-assets/icons> --undo   # flatten again
"""
import os
import sys
import zlib

BUCKETS = 16


def bucket_for(name):
    return "%02x" % (zlib.crc32(name.lower().encode("utf-8")) % BUCKETS)


def split(root):
    moved = skipped = 0
    for name in sorted(os.listdir(root)):
        src = os.path.join(root, name)
        if not os.path.isfile(src):
            skipped += 1
            continue
        dst_dir = os.path.join(root, bucket_for(name))
        os.makedirs(dst_dir, exist_ok=True)
        dst = os.path.join(dst_dir, name)
        if os.path.exists(dst):
            print("  skip (already there): %s" % name)
            skipped += 1
            continue
        os.replace(src, dst)
        moved += 1
    print("moved %d files into %d buckets (%d skipped)" % (moved, BUCKETS, skipped))


def undo(root):
    moved = 0
    for b in sorted(os.listdir(root)):
        d = os.path.join(root, b)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            src = os.path.join(d, name)
            dst = os.path.join(root, name)
            if os.path.exists(dst):
                print("  skip (exists at top): %s" % name)
                continue
            os.replace(src, dst)
            moved += 1
        try:
            os.rmdir(d)
        except OSError:
            pass
    print("restored %d files to %s" % (moved, root))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    target = sys.argv[1]
    if not os.path.isdir(target):
        sys.exit("not a directory: %s" % target)
    if "--undo" in sys.argv:
        undo(target)
    else:
        split(target)
        print("")
        print("NOTE: if you ran this directly on the SD card, the result will be")
        print("      SLOWER than not splitting at all. Delete assets/icons/ on")
        print("      the card and copy this split folder back onto it fresh.")
