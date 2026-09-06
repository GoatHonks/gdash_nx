/* asset_index.c -- in-memory index of the assets tree.
 *
 * Horizon's filesystem resolves a path by scanning the directory linearly over
 * IPC, so with ~1000 entries in assets/ a single open costs tens of
 * milliseconds. cocos2d's fullPathForFilename probes several candidate paths
 * per sprite (search paths x -hd/-uhd suffixes) and most of those probes MISS,
 * so the game pays that scan repeatedly, on the render thread, mid-frame.
 *
 * The assets tree is static for the life of the process, so we walk it once at
 * startup and answer "does this exist?" from RAM. A miss then costs a hash
 * lookup instead of a directory scan. We only ever fail fast -- we never
 * fabricate a success -- so a stale or incomplete index can slow things back
 * down but cannot make a present file unreadable.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "asset_index.h"

#define SLOTS 16384u // power of two, >2x the ~6150 entries we expect

static char *g_slot[SLOTS];

// Alias table: maps the path the game ASKS for onto where the file actually
// lives, so assets/icons/ can be split into subdirectories to shrink the
// per-open directory scan without the game knowing. "icons/foo.png" ->
// "icons/0a/foo.png". Only consulted when the literal path is absent, so an
// unsplit install behaves exactly as it did before.
typedef struct { char *from; char *to; } Alias;
static Alias g_alias[SLOTS];
static unsigned g_alias_n;
static char g_root[512];
static size_t g_root_len;
static int g_ready;
static unsigned g_miss_count;
static unsigned g_stats;      // stat() calls spent building
static unsigned g_build_ms;
static double g_deadline;
static int g_timed_out;
static unsigned g_count;

static unsigned hash_ci(const char *s) {
  unsigned h = 2166136261u; // FNV-1a, case-folded
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c == 0x5C) c = '/';
    h = (h ^ c) * 16777619u;
  }
  return h;
}

static int same_ci(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x == 0x5C) x = '/';
    if (y == 0x5C) y = '/';
    if (x != y) return 0;
  }
  return *a == 0 && *b == 0;
}

static void add(const char *rel) {
  unsigned i = hash_ci(rel) & (SLOTS - 1u);
  for (unsigned n = 0; n < SLOTS; n++) {
    if (!g_slot[i]) {
      g_slot[i] = strdup(rel);
      if (g_slot[i]) g_count++;
      return;
    }
    if (same_ci(g_slot[i], rel))
      return;
    i = (i + 1u) & (SLOTS - 1u);
  }
}

static int contains(const char *rel) {
  unsigned i = hash_ci(rel) & (SLOTS - 1u);
  for (unsigned n = 0; n < SLOTS; n++) {
    if (!g_slot[i]) return 0;
    if (same_ci(g_slot[i], rel)) return 1;
    i = (i + 1u) & (SLOTS - 1u);
  }
  return 0;
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Is this entry a directory, WITHOUT calling stat()? On Horizon every stat()
// is a full linear directory scan -- the exact cost this index exists to
// remove -- so doing one per entry costs minutes over a 6000-file tree and
// hangs startup. readdir's d_type answers for free where the platform fills
// it in; otherwise we lean on the fact that asset directories carry no file
// extension, which reduces thousands of stat() calls to a handful.
static int entry_is_dir(struct dirent *e, const char *full) {
#ifdef DT_DIR
  if (e->d_type == DT_DIR)
    return 1;
  if (e->d_type == DT_REG)
    return 0;
#endif
  if (strchr(e->d_name, '.'))
    return 0; // has an extension: a file, and no syscall spent deciding
  struct stat st;
  g_stats++;
  return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

// depth-limited so a symlink loop or a surprise deep tree cannot hang startup
static void walk(const char *dir, const char *prefix, int depth) {
  if (depth > 8 || g_timed_out)
    return;
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  unsigned seen = 0;
  while ((e = readdir(d)) != NULL) {
    // a slow filesystem must degrade to "no index", never to a hung launch
    if ((++seen & 63u) == 0 && now_sec() > g_deadline) {
      g_timed_out = 1;
      break;
    }
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
      continue;
    char rel[512], full[768];
    if (prefix[0])
      snprintf(rel, sizeof(rel), "%s/%s", prefix, e->d_name);
    else
      snprintf(rel, sizeof(rel), "%s", e->d_name);
    snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
    add(rel);
    if (entry_is_dir(e, full))
      walk(full, rel, depth + 1);
  }
  closedir(d);
}

static void alias_add(const char *from, const char *to) {
  unsigned i = hash_ci(from) & (SLOTS - 1u);
  for (unsigned n = 0; n < SLOTS; n++) {
    if (!g_alias[i].from) {
      g_alias[i].from = strdup(from);
      g_alias[i].to = strdup(to);
      if (g_alias[i].from && g_alias[i].to) g_alias_n++;
      return;
    }
    if (same_ci(g_alias[i].from, from)) {
      g_alias[i].to = NULL; // two files claim this alias: refuse to guess
      return;
    }
    i = (i + 1u) & (SLOTS - 1u);
  }
}

static const char *alias_find(const char *from) {
  unsigned i = hash_ci(from) & (SLOTS - 1u);
  for (unsigned n = 0; n < SLOTS; n++) {
    if (!g_alias[i].from) return NULL;
    if (same_ci(g_alias[i].from, from)) return g_alias[i].to;
    i = (i + 1u) & (SLOTS - 1u);
  }
  return NULL;
}

// "icons/0a/foo.png" is also reachable as "icons/foo.png": same first segment,
// same filename. Deeper nesting collapses the same way.
static void build_aliases(void) {
  for (unsigned i = 0; i < SLOTS; i++) {
    const char *rel = g_slot[i];
    if (!rel) continue;
    const char *first = strchr(rel, '/');
    if (!first) continue;        // root-level file: nothing to alias
    const char *base = strrchr(rel, '/');
    if (base == first) continue; // already only one level deep
    char from[512];
    size_t seg = (size_t)(first - rel);
    if (seg + strlen(base) + 1 >= sizeof(from)) continue;
    memcpy(from, rel, seg);
    snprintf(from + seg, sizeof(from) - seg, "%s", base);
    alias_add(from, rel);
  }
}

void asset_index_build(const char *root) {
  if (g_ready || !root || !root[0])
    return;
  snprintf(g_root, sizeof(g_root), "%s", root);
  g_root_len = strlen(g_root);
  while (g_root_len > 1 && g_root[g_root_len - 1] == '/')
    g_root[--g_root_len] = 0;
  const double t0 = now_sec();
  g_deadline = t0 + 4.0; // hard ceiling: never trade a hang for a speedup
  walk(g_root, "", 0);
  build_aliases();
  g_build_ms = (unsigned)((now_sec() - t0) * 1000.0);
  // Trust the index only if the walk finished AND found a real tree. A partial
  // walk would report whole subtrees as missing; an empty one would report
  // everything as missing and render a black screen.
  g_ready = (!g_timed_out && g_count > 64);
}

unsigned asset_index_count(void) { return g_count; }

int asset_index_active(void) { return g_ready; }

unsigned asset_index_build_ms(void) { return g_build_ms; }

unsigned asset_index_stats(void) { return g_stats; }

// A "." or ".." segment (or an empty one from "//") means the literal path we
// would look up is not the path that will be opened, so the index cannot
// answer for it. Saying "unknown" costs one slow lookup; saying "missing"
// would hide a real file.
static int unnormalized(const char *rel) {
  if (strstr(rel, "//"))
    return 1;
  for (const char *p = rel; *p; p++) {
    if (*p != '.' || (p != rel && p[-1] != '/'))
      continue;
    if (p[1] == '/' || p[1] == 0)
      return 1;
    if (p[1] == '.' && (p[2] == '/' || p[2] == 0))
      return 1;
  }
  return 0;
}

// If `path` does not exist literally but the same filename exists one level
// deeper under the same top folder, return its real absolute path. Rotating
// buffers so concurrent callers cannot clobber each other.
const char *asset_index_resolve(const char *path) {
  static char buf[4][768];
  static unsigned turn;
  if (!g_ready || !path)
    return NULL;
  if (strncasecmp(path, g_root, g_root_len) != 0 || path[g_root_len] != '/')
    return NULL;
  const char *rel = path + g_root_len;
  while (*rel == '/') rel++;
  if (!*rel || unnormalized(rel) || contains(rel))
    return NULL; // absent from the tree, or already correct as written
  const char *to = alias_find(rel);
  if (!to)
    return NULL;
  char *out = buf[turn++ & 3u];
  snprintf(out, sizeof(buf[0]), "%s/%s", g_root, to);
  return out;
}

unsigned asset_index_aliases(void) { return g_alias_n; }

unsigned asset_index_slots(void) { return SLOTS; }

// Relative path held in slot i, or NULL if that slot is empty. Lets the
// prefetcher walk the tree without a second directory scan.
const char *asset_index_at(unsigned i) {
  return (i < SLOTS) ? g_slot[i] : NULL;
}

int asset_index_missing(const char *path) {
  if (!g_ready || !path)
    return 0; // no index: let the filesystem answer
  if (strncasecmp(path, g_root, g_root_len) != 0)
    return 0; // outside the assets tree: not ours to judge
  const char *rel = path + g_root_len;
  while (*rel == '/')
    rel++;
  if (!*rel)
    return 0; // the root directory itself
  if (unnormalized(rel))
    return 0;
  if (contains(rel))
    return 0;

  // The index claims this is missing. Audit a small sample of those claims
  // against the real filesystem, forever rather than just at startup: if the
  // walk silently missed a subtree (say stat() on directories failed) the
  // error would not show up until that subtree is first requested. One check
  // per 128 misses costs ~1% of what we are saving and disables the index the
  // moment it is caught lying, so a wrong index degrades to slow, never to
  // invisible files.
  if ((g_miss_count++ & 127u) == 0) {
    struct stat st;
    if (stat(path, &st) == 0) {
      g_ready = 0; // index is wrong -- fall back to the filesystem for good
      return 0;
    }
  }
  return 1;
}
