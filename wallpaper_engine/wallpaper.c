#include "wallpaper.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

typedef struct {
    GdkPixbuf *pixbuf;
    GtkWidget *drawing_area;
} WallpaperData;

static gboolean on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    WallpaperData *data = (WallpaperData *)user_data;

    // Fill background with pure black before fading in wallpaper
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    if (!data->pixbuf) return FALSE;

    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    cairo_save(cr);
    cairo_scale(cr,
        (double)width / gdk_pixbuf_get_width(data->pixbuf),
        (double)height / gdk_pixbuf_get_height(data->pixbuf));
    gdk_cairo_set_source_pixbuf(cr, data->pixbuf, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    
    return FALSE;
}

static void on_destroy(GtkWidget *widget, gpointer user_data) {
    WallpaperData *data = (WallpaperData *)user_data;
    if (data->pixbuf) g_object_unref(data->pixbuf);
    g_free(data);
}

gboolean wallpaper_window_set_image(GtkWidget *window, const char *image_path,
                                    GError **error) {
    WallpaperData *data = g_object_get_data(G_OBJECT(window), "wallpaper-data");
    if (!data) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "The window is not a macOS-DE wallpaper window.");
        return FALSE;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(image_path, error);
    if (!pixbuf)
        return FALSE;

    g_clear_object(&data->pixbuf);
    data->pixbuf = pixbuf;
    gtk_widget_queue_draw(data->drawing_area);
    return TRUE;
}

GtkWidget* create_wallpaper_window(const char *image_path, GError **error) {
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(image_path, error);
    if (!pixbuf) return NULL;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "macOS DE - Wallpaper");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DESKTOP);

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    if (monitor) {
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        gtk_window_set_default_size(GTK_WINDOW(window), geometry.width, geometry.height);
        gtk_window_move(GTK_WINDOW(window), geometry.x, geometry.y);
    } else {
        gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);
    }

    WallpaperData *data = g_new0(WallpaperData, 1);
    data->pixbuf = pixbuf;
    g_object_set_data(G_OBJECT(window), "wallpaper-data", data);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *drawing_area = gtk_drawing_area_new();
    data->drawing_area = drawing_area;
    gtk_container_add(GTK_CONTAINER(overlay), drawing_area);
    gtk_container_add(GTK_CONTAINER(window), overlay);

    g_signal_connect(drawing_area, "draw", G_CALLBACK(on_draw_event), data);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), data);

    return window;
}