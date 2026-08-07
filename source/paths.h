/* paths.h -- runtime file layout, resolved from the .nro's own location so the
 * game folder can be named anything (not tied to a fixed /switch/<name>/).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef PATHS_H
#define PATHS_H

// Resolve the base directory from argv[0] (else the cwd). Call once at startup.
void paths_init(const char *argv0);

const char *path_base(void);          // directory of the .nro
const char *path_assets(void);        // "<base>/assets"
const char *path_assets_search(void); // "<base>/assets/" (cocos search path)
const char *path_save(void);          // "<base>/save"
const char *path_prefs(void);         // "<base>/prefs.txt"
const char *path_config(void);        // "<base>/config.txt"
const char *path_ca_bundle(void);     // "<base>/cacert.pem"
const char *path_so_game(void);       // "<base>/libcocos2dcpp.so"
const char *path_so_fmod(void);       // "<base>/libfmod.so"

// Return the part after an Android app-private package directory, or NULL
// when path is not under a recognized private-data root. The returned pointer
// aliases path and may point at its trailing NUL for the package directory.
const char *path_android_private_suffix(const char *path);

#endif
