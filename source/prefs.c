/* prefs.c -- persistent key/value store backing the SharedPreferences JNI
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prefs.h"

#define MAX_PREFS 128
#define KEY_LEN 96
#define VAL_LEN 256

typedef struct {
  char key[KEY_LEN];
  char val[VAL_LEN];
} Pref;

static Pref prefs[MAX_PREFS];
static int pref_count = 0;
static char prefs_file[512];

static Pref *find(const char *key) {
  for (int i = 0; i < pref_count; i++)
    if (!strcmp(prefs[i].key, key))
      return &prefs[i];
  return NULL;
}

static void save(void) {
  if (!prefs_file[0])
    return;
  FILE *f = fopen(prefs_file, "w");
  if (!f)
    return;
  for (int i = 0; i < pref_count; i++)
    fprintf(f, "%s\t%s\n", prefs[i].key, prefs[i].val);
  fclose(f);
}

static void set_raw(const char *key, const char *val) {
  if (!val)
    val = "";
  Pref *p = find(key);
  if (p && !strcmp(p->val, val))
    return; // unchanged: skip the file rewrite (callers can be per-frame)
  if (!p) {
    if (pref_count >= MAX_PREFS)
      return;
    p = &prefs[pref_count++];
    snprintf(p->key, sizeof(p->key), "%s", key);
  }
  snprintf(p->val, sizeof(p->val), "%s", val);
  save();
}

void prefs_load(const char *path) {
  snprintf(prefs_file, sizeof(prefs_file), "%s", path);
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[KEY_LEN + VAL_LEN + 8];
  while (fgets(line, sizeof(line), f) && pref_count < MAX_PREFS) {
    char *tab = strchr(line, '\t');
    if (!tab)
      continue;
    *tab = '\0';
    char *val = tab + 1;
    char *nl = strchr(val, '\n');
    if (nl)
      *nl = '\0';
    Pref *p = &prefs[pref_count++];
    snprintf(p->key, sizeof(p->key), "%s", line);
    snprintf(p->val, sizeof(p->val), "%s", val);
  }
  fclose(f);
}

int prefs_get_int(const char *key, int def) {
  Pref *p = find(key);
  return p ? atoi(p->val) : def;
}

int prefs_get_bool(const char *key, int def) {
  Pref *p = find(key);
  return p ? (atoi(p->val) != 0) : def;
}

float prefs_get_float(const char *key, float def) {
  Pref *p = find(key);
  return p ? strtof(p->val, NULL) : def;
}

double prefs_get_double(const char *key, double def) {
  Pref *p = find(key);
  return p ? strtod(p->val, NULL) : def;
}

const char *prefs_get_string(const char *key, const char *def) {
  Pref *p = find(key);
  return p ? p->val : def;
}

void prefs_set_int(const char *key, int v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", v);
  set_raw(key, buf);
}

void prefs_set_bool(const char *key, int v) {
  set_raw(key, v ? "1" : "0");
}

void prefs_set_float(const char *key, float v) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.9g", v);
  set_raw(key, buf);
}

void prefs_set_double(const char *key, double v) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.17g", v);
  set_raw(key, buf);
}

void prefs_set_string(const char *key, const char *v) {
  set_raw(key, v);
}
