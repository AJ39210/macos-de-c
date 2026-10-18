#ifndef STARTUP_H
#define STARTUP_H

#include <gtk/gtk.h>

typedef void (*StartupCallback)(gboolean success, const char *error_message,
                                gpointer user_data);

// Plays frames 00 to 59 truthfully during initialization
void start_system_boot(const char *base_path, StartupCallback callback, gpointer user_data);

#endif