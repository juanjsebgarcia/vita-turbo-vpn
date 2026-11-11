/*
 * paf_task.h - run a function on SceShell's PAF main (UI) thread once per
 * frame: the call QuickMenuReborn itself uses for its open/close checks.
 *
 * In PAF these are paf::common::Utils::AddMainThreadTask and
 * RemoveMainThreadTask (static members, so plain C calls): the task is
 * called as fn(arg) from the UI thread's frame loop until removed. Both
 * may be called from inside a task, including a task removing itself
 * (QuickMenuReborn's CheckMenuCloseTask does exactly that).
 *
 * NIDs (qmr/paf_imports.yml), library ScePafCommon 0x3C1965CD:
 *   Add    0x6735B503  (QuickMenuReborn's impose_thread: delay 4 s, then
 *                       Add(ImposeCheck, NULL), then exit)
 *   Remove 0x4A8115D5  (its module_stop: Remove(ImposeCheck, NULL))
 * Both are exported by libpaf.suprx on every firmware 3.60-3.74
 * (the same on each).
 */
#ifndef VTW_PAF_TASK_H
#define VTW_PAF_TASK_H

typedef void (*PafTaskFn)(void *arg);

int scePafAddMainThreadTask(PafTaskFn fn, void *arg);
int scePafRemoveMainThreadTask(PafTaskFn fn, void *arg);

#endif /* VTW_PAF_TASK_H */
