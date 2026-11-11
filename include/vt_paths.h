/*
 * vt_paths.h - the folders the VPN keeps its files in, named once.
 *
 *   VT_DATA_DIR      the modules' own data: options, logs, the boot guard,
 *                    the last profile used.
 *   VT_PROFILE_DIR   the profiles and the app's files, under a device
 *                    ("ux0:" VT_PROFILE_DIR "/profiles").
 *
 * Former names. If the product is ever renamed, the folders the old name used
 * are added to the lists below (strings, each followed by a comma), and
 * nothing else changes:
 *   - at start, a former folder is renamed to the current name when the
 *     current one does not exist yet (the kernel module does the data folder
 *     before it writes anything; the app does the profile folder on ux0: and
 *     ur0:);
 *   - a saved path that starts with a former folder (the last profile used)
 *     is read from the current one (vt_path_current).
 * Both lists are empty today.
 */
#ifndef VT_PATHS_H
#define VT_PATHS_H

#include <string.h>

#define VT_DATA_DIR    "ur0:data/vpnturbo"
#define VT_PROFILE_DIR "data/vita-turbo-vpn"

/* Former data folders, full paths ("ur0:data/<old>",). */
#define VT_LEGACY_DATA_DIRS
/* Former profile folders, without the device ("data/<old>",). */
#define VT_LEGACY_PROFILE_DIRS

/*
 * Rewrite `path` in place when it starts with a former profile folder
 * ("<dev>:data/<old>/...") or a former data folder, so it names the same file
 * in the current folder. Returns 1 if it was rewritten, 0 if not (or if the
 * result would not fit in `size`).
 */
static inline int
vt_path_current(char *path, unsigned size)
{
	static const char *const old_prof[] = { VT_LEGACY_PROFILE_DIRS 0 };
	static const char *const old_data[] = { VT_LEGACY_DATA_DIRS 0 };
	const char *colon = strchr(path, ':');
	unsigned i;

	for (i = 0; old_data[i]; i++) {
		unsigned ol = (unsigned)strlen(old_data[i]);
		unsigned nl = (unsigned)strlen(VT_DATA_DIR);
		unsigned pl = (unsigned)strlen(path);
		if (strncmp(path, old_data[i], ol) == 0 && (path[ol] == '/' || path[ol] == 0) &&
		    pl - ol + nl < size) {
			memmove(path + nl, path + ol, pl - ol + 1);
			memcpy(path, VT_DATA_DIR, nl);
			return 1;
		}
	}
	if (!colon)
		return 0;
	for (i = 0; old_prof[i]; i++) {
		char *rest = (char *)colon + 1;
		unsigned ol = (unsigned)strlen(old_prof[i]);
		unsigned nl = (unsigned)strlen(VT_PROFILE_DIR);
		unsigned pl = (unsigned)strlen(path);
		unsigned at = (unsigned)(rest - path);
		if (strncmp(rest, old_prof[i], ol) == 0 && (rest[ol] == '/' || rest[ol] == 0) &&
		    pl - ol + nl < size) {
			memmove(rest + nl, rest + ol, pl - at - ol + 1);
			memcpy(rest, VT_PROFILE_DIR, nl);
			return 1;
		}
	}
	return 0;
}

#endif /* VT_PATHS_H */
