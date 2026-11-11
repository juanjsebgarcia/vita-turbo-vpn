/*
 * qmr.h - the QuickMenuReborn calls the quick-menu rows make (the same
 * declarations Wi-Fi Turbo uses).
 *
 * QuickMenuReborn (https://github.com/Ibrahim778/QuickMenuReborn, MIT) is a
 * separately installed SceShell plugin that exports a "QuickMenuReborn"
 * library; only the functions used here are declared. qmr_imports.yml lists
 * their NIDs for the link stub.
 */
#ifndef VTW_QMR_H
#define VTW_QMR_H

#include <psp2/types.h>

/* Widget kinds for QuickMenuRebornRegisterWidget. */
#define QMR_BUTTON      0
#define QMR_CHECK_BOX   1
#define QMR_TEXT        2
#define QMR_PLANE       3

#define QMR_EVENT_RELEASE     0x10000008   /* button / check box released     */
#define QMR_ROW_WIDTH         835.0f       /* width of Sony's quick-menu rows */
#define QMR_SEPARATOR_HEIGHT  20.0f

typedef void (*QmrEventHandler)(const char *id, SceInt32 hash, SceInt32 event, void *user);
typedef void (*QmrIdHandler)(const char *id);

/* Widgets are keyed by string id; they are (re)built each time the menu opens,
 * in registration order, under `parent` (NULL = the menu itself). */
void *QuickMenuRebornRegisterWidget(const char *id, const char *parent, int kind);
int   QuickMenuRebornUnregisterWidget(const char *id);
int   QuickMenuRebornSeparator(const char *id, float height);
int   QuickMenuRebornRemoveSeparator(const char *id);

int   QuickMenuRebornSetWidgetSize(const char *id, float x, float y, float z, float w);
int   QuickMenuRebornSetWidgetPosition(const char *id, float x, float y, float z, float w);
int   QuickMenuRebornSetWidgetColor(const char *id, float r, float g, float b, float a);
int   QuickMenuRebornSetWidgetLabel(const char *id, const char *label);

int   QuickMenuRebornRegisterEventHanlder(const char *id, SceInt32 event, QmrEventHandler fn, void *user);
int   QuickMenuRebornAssignOnLoadHandler(QmrIdHandler fn, const char *id);

/* Called for every registered widget when QuickMenuReborn sees the menu has
 * closed (its exit button is gone), on SceShell's PAF main thread, after the
 * widgets are destroyed. Exported by QuickMenuReborn 2.5 (checked in the
 * released binary). */
int   QuickMenuRebornAssignOnDeleteHandler(QmrIdHandler fn, const char *id);

/* Check box: the live state while the menu is open, else the saved one. The
 * default recall ticks the box from the saved state as it is built. */
int   QuickMenuRebornGetCheckboxValue(const char *id);
int   QuickMenuRebornSaveCheckBoxState(const char *id, int state);
int   QuickMenuRebornAssignDefaultCheckBoxRecall(const char *id);

/* A widget built from a named PAF style instead of QuickMenuReborn's own
 * template; type is the widget class name, e.g. "text". */
void *QuickMenuRebornRegisterWidgetFromStyle(const char *id, const char *parent, const char *type, const char *style);

/* Textures: an image file QuickMenuReborn loads when the menu opens. */
int   QuickMenuRebornRegisterTexture(const char *id, const char *path);
int   QuickMenuRebornUnregisterTexture(const char *id);
int   QuickMenuRebornSetWidgetTexture(const char *id, const char *texture_id);

#endif /* VTW_QMR_H */
