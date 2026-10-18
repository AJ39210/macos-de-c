#ifndef MACOSWM_H
#define MACOSWM_H

typedef struct MacosWm MacosWm;

/* Starts an ICCCM/EWMH-compatible X11 window manager on the selected display. */
MacosWm *macos_wm_new(const char *display_name);
int macos_wm_run(MacosWm *wm);
void macos_wm_free(MacosWm *wm);

#endif
