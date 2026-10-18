#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <gtk/gtk.h>

// Creates a background window/widget that draws the wallpaper image
GtkWidget* create_wallpaper_window(const char *image_path, GError **error);
gboolean wallpaper_window_set_image(GtkWidget *window, const char *image_path,
                                    GError **error);

#endif