/* prefs.h -- persistent key/value store backing the SharedPreferences JNI
 * (Cocos2dxHelper get/setXForKey). One "key value" line per entry.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __PREFS_H__
#define __PREFS_H__

void prefs_load(const char *path);

// getters return the caller's default when the key is missing
int prefs_get_int(const char *key, int def);
int prefs_get_bool(const char *key, int def);
float prefs_get_float(const char *key, float def);
double prefs_get_double(const char *key, double def);
const char *prefs_get_string(const char *key, const char *def);

// setters persist immediately (writes are rare: options screen only)
void prefs_set_int(const char *key, int v);
void prefs_set_bool(const char *key, int v);
void prefs_set_float(const char *key, float v);
void prefs_set_double(const char *key, double v);
void prefs_set_string(const char *key, const char *v);

#endif
