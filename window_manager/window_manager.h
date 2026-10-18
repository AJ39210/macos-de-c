#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <gtk/gtk.h>

// Creates a custom macOS-styled window with traffic light buttons and draggable header
GtkWidget* mac_window_new(const char *title, int width, int height);

// Adds child content to the window's body area below the custom titlebar
void mac_window_set_child(GtkWidget *mac_window, GtkWidget *child);

#endif