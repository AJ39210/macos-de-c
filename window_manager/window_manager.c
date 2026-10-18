#include "window_manager.h"

static gboolean on_titlebar_button_press(GtkWidget *widget, GdkEventButton *event, gpointer window) {
    if (event->button == GDK_BUTTON_PRIMARY) {
        // Double-click on titlebar toggles maximize/unmaximize
        if (event->type == GDK_2BUTTON_PRESS) {
            GdkWindow *gdk_win = gtk_widget_get_window(GTK_WIDGET(window));
            if (gdk_window_get_state(gdk_win) & GDK_WINDOW_STATE_MAXIMIZED) {
                gtk_window_unmaximize(GTK_WINDOW(window));
            } else {
                gtk_window_maximize(GTK_WINDOW(window));
            }
            return TRUE;
        }

        // Single click initiates smooth window dragging
        if (event->type == GDK_BUTTON_PRESS) {
            gtk_window_begin_move_drag(
                GTK_WINDOW(window),
                event->button,
                (gint)event->x_root,
                (gint)event->y_root,
                event->time
            );
            return TRUE;
        }
    }
    return FALSE;
}

static void on_close_clicked(GtkButton *btn, gpointer window) {
    gtk_widget_destroy(GTK_WIDGET(window));
}

static void on_minimize_clicked(GtkButton *btn, gpointer window) {
    gtk_window_iconify(GTK_WINDOW(window));
}

static void on_maximize_clicked(GtkButton *btn, gpointer window) {
    GdkWindow *gdk_win = gtk_widget_get_window(GTK_WIDGET(window));
    if (gdk_window_get_state(gdk_win) & GDK_WINDOW_STATE_MAXIMIZED) {
        gtk_window_unmaximize(GTK_WINDOW(window));
    } else {
        gtk_window_maximize(GTK_WINDOW(window));
    }
}

GtkWidget* mac_window_new(const char *title_text, int width, int height) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), width, height);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_widget_set_app_paintable(window, TRUE);

    GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), root_vbox);

    // Custom macOS Titlebar
    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_size_request(titlebar, -1, 32);
    gtk_style_context_add_class(gtk_widget_get_style_context(titlebar), "mac-titlebar");
    gtk_box_pack_start(GTK_BOX(root_vbox), titlebar, FALSE, FALSE, 0);

    // Traffic Lights Container (Left)
    GtkWidget *traffic_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(traffic_box, 10);
    gtk_box_pack_start(GTK_BOX(titlebar), traffic_box, FALSE, FALSE, 0);

    // Close Button (Red)
    GtkWidget *btn_close = gtk_button_new();
    gtk_widget_set_size_request(btn_close, 12, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_close), "traffic-close");
    gtk_container_add(GTK_CONTAINER(traffic_box), btn_close);
    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_close_clicked), window);

    // Minimize Button (Yellow)
    GtkWidget *btn_min = gtk_button_new();
    gtk_widget_set_size_request(btn_min, 12, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_min), "traffic-minimize");
    gtk_container_add(GTK_CONTAINER(traffic_box), btn_min);
    g_signal_connect(btn_min, "clicked", G_CALLBACK(on_minimize_clicked), window);

    // Maximize Button (Green)
    GtkWidget *btn_max = gtk_button_new();
    gtk_widget_set_size_request(btn_max, 12, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_max), "traffic-maximize");
    gtk_container_add(GTK_CONTAINER(traffic_box), btn_max);
    g_signal_connect(btn_max, "clicked", G_CALLBACK(on_maximize_clicked), window);

    // Centered Title Label
    GtkWidget *title_label = gtk_label_new(title_text);
    gtk_widget_set_hexpand(title_label, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(title_label), "mac-title-label");
    gtk_box_pack_start(GTK_BOX(titlebar), title_label, TRUE, TRUE, 0);

    // Right spacer to perfectly center the title relative to traffic lights
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(spacer, 60, -1);
    gtk_box_pack_start(GTK_BOX(titlebar), spacer, FALSE, FALSE, 0);

    // Enable drag events and double-click on titlebar
    gtk_widget_set_events(titlebar, GDK_BUTTON_PRESS_MASK | GDK_2BUTTON_PRESS);
    g_signal_connect(titlebar, "button-press-event", G_CALLBACK(on_titlebar_button_press), window);

    return window;
}

void mac_window_set_child(GtkWidget *mac_window, GtkWidget *child) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(mac_window));
    if (children && GTK_IS_CONTAINER(children->data)) {
        GtkWidget *root_vbox = GTK_WIDGET(children->data);
        gtk_box_pack_start(GTK_BOX(root_vbox), child, TRUE, TRUE, 0);
    }
    g_list_free(children);
}