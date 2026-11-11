
/*
 * log.h - small logger shared by both instances (SceShell and Settings).
 *
 * Appends timestamped lines to ur0:data/vpnturbo/shell.log,
 * rotated to shell.old.log past VTW_LOG_MAX_BYTES. Each line opens, appends
 * and closes the file, so nothing stays open; a line that cannot be written
 * is dropped. Lines are written on start, state changes and user actions
 * only, never per poll. Never logs key material.
 *
 * printf subset: %s %d %u %x %X %c %%, with an optional zero-pad width.
 */
#ifndef VTW_LOG_H
#define VTW_LOG_H

#include "vt_paths.h"

#define VTW_DATA_DIR       VT_DATA_DIR
#define VTW_LOG_PATH       VTW_DATA_DIR "/shell.log"
#define VTW_LOG_PATH_OLD   VTW_DATA_DIR "/shell.old.log"
#define VTW_LOG_MAX_BYTES  (32 * 1024)

/* "shell" or "settings", set once by module_start. */
void vtw_log_set_tag(const char *tag);
void vtw_log(const char *fmt, ...);

#define VTW_LOG(...) vtw_log(__VA_ARGS__)

#endif /* VTW_LOG_H */
