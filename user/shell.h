/*
 * shell.h - the SceShell instance: quick-menu rows, the drop watcher
 * and the lent transport threads.
 */
#ifndef VTW_SHELL_H
#define VTW_SHELL_H

void vtw_shell_start(void);
void vtw_shell_stop(void);

/* Load the last-used profile into the kernel without connecting. */
void vtw_preload_last(void);

#endif /* VTW_SHELL_H */
