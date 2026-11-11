/*
 * notify.h - the drop notification, sent from SceShell.
 */
#ifndef VTW_NOTIFY_H
#define VTW_NOTIFY_H

/* Start the sender thread (the library is loaded at the first send). */
void vtw_notify_init(void);

/* Queue "VPN dropped - traffic is not protected" in the current system
 * language. Returns at once; the notification is sent on its own thread. */
void vtw_notify_drop(void);

void vtw_notify_fini(void);

#endif /* VTW_NOTIFY_H */
