/* paths.c -- see paths.h.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "config.h" // SO_NAME, FMOD_SO_NAME, CONFIG_NAME
#include "paths.h"

static char s_base[256], s_assets[288], s_assets_slash[288], s_save[288], s_prefs[288];
static char s_config[288], s_so_game[288], s_so_fmod[288];

// strip a "device:" prefix and the trailing "/<file>" component
static void dir_of(const char *in, char *out, size_t n) {
  const char *colon = strchr(in, ':');
  snprintf(out, n, "%s", colon ? colon + 1 : in);
  char *slash = strrchr(out, '/');
  if (slash)
    *slash = '\0';
}

void paths_init(const char *argv0) {
  s_base[0] = '\0';
  if (argv0 && argv0[0])
    dir_of(argv0, s_base, sizeof s_base);
  if (!s_base[0]) {
    char cwd[256];
    if (getcwd(cwd, sizeof cwd)) {
      const char *colon = strchr(cwd, ':');
      snprintf(s_base, sizeof s_base, "%s", colon ? colon + 1 : cwd);
    }
  }
  if (!s_base[0])
    snprintf(s_base, sizeof s_base, "/switch/gdash");
  if (s_base[0] != '/') { // cocos only treats '/'-rooted paths as filesystem paths
    char tmp[256];
    snprintf(tmp, sizeof tmp, "/%s", s_base);
    snprintf(s_base, sizeof s_base, "%s", tmp);
  }
  snprintf(s_assets,       sizeof s_assets,       "%s/assets", s_base);
  snprintf(s_assets_slash, sizeof s_assets_slash, "%s/assets/", s_base);
  snprintf(s_save,         sizeof s_save,         "%s/save", s_base);
  snprintf(s_prefs,        sizeof s_prefs,        "%s/prefs.txt", s_base);
  snprintf(s_config,       sizeof s_config,       "%s/%s", s_base, CONFIG_NAME);
  snprintf(s_so_game,      sizeof s_so_game,      "%s/%s", s_base, SO_NAME);
  snprintf(s_so_fmod,      sizeof s_so_fmod,      "%s/%s", s_base, FMOD_SO_NAME);
}

const char *path_base(void)          { return s_base; }
const char *path_assets(void)        { return s_assets; }
const char *path_assets_search(void) { return s_assets_slash; }
const char *path_save(void)          { return s_save; }
const char *path_prefs(void)         { return s_prefs; }
const char *path_config(void)        { return s_config; }
const char *path_so_game(void)       { return s_so_game; }
const char *path_so_fmod(void)       { return s_so_fmod; }
