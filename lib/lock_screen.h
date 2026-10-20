#ifndef MACOS_DE_LOCK_SCREEN_H
#define MACOS_DE_LOCK_SCREEN_H

#include <gtk/gtk.h>

typedef struct MacosDeLockScreen MacosDeLockScreen;

MacosDeLockScreen *macos_de_lock_screen_new(
    GtkWidget *topbar_window, GtkWidget *power_button, GtkWidget *topbar_clock,
    GtkWidget *active_app_label, GtkWidget *status_icons,
    const char *default_wallpaper);
gboolean macos_de_lock_screen_lock_now(MacosDeLockScreen *lock_screen,
                                       GError **error);
gboolean macos_de_lock_screen_should_lock_on_startup(
    const MacosDeLockScreen *lock_screen);
void macos_de_lock_screen_free(MacosDeLockScreen *lock_screen);

#endif
