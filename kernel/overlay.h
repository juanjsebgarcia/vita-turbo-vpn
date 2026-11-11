/*
 * overlay.h - the framebuffer overlay: status-bar padlock and
 * HUD. See overlay.c.
 */
#ifndef TN_OVERLAY_H
#define TN_OVERLAY_H

/* Module start: read the settings file and hook the framebuffer. Nothing
 * is drawn until a client has called vtSetOverlay once (the SceShell plugin
 * does at boot). A failed hook is logged; the module carries on. */
void overlay_init(void);
void overlay_fini(void);

/* Rebuild the HUD text if what it shows changed (tunnel state, tunnel or
 * exit address, language, mode), and drop the exit address while the tunnel
 * is idle. Cheap when nothing changed; call it after anything that changes
 * the tunnel's state, and from the timer tick. Never from the framebuffer
 * hook. */
void overlay_hud_sync(void);

/* Forget the exit address (a new connection is starting). */
void overlay_exit_ip_reset(void);

#endif /* TN_OVERLAY_H */
