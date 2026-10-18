#ifndef MACOS_DE_SYSTEM_STATUS_H
#define MACOS_DE_SYSTEM_STATUS_H

#include <gtk/gtk.h>

GtkWidget *system_status_create(GtkWidget *power_button,
                                GCallback logout_callback,
                                GCallback reboot_callback,
                                GCallback shutdown_callback,
                                const char *asset_directory,
                                gboolean dark_theme);

#endif
