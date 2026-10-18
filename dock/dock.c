#include "dock.h"
#include "cursor_animator.h"
#include "../lib/dock_settings.h"
#include <gdk/gdkx.h>
#include <gio/gdesktopappinfo.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    Window window;
    GtkWidget *button;
    char *class_name;
    gboolean seen;
    Window *window_data;
} RunningApp;

typedef struct {
    GtkWidget *button;
    GtkWidget *icon;
    GtkWidget *indicator;
    char *wm_class;
    char *icon_path;
} PinnedLauncher;

typedef struct {
    GtkWidget *dock_box;
    GtkWidget *finder_indicator;
    GtkWidget *file_indicator;
    GtkWidget *terminal_indicator;
    GtkWidget *settings_indicator;
    GPtrArray *running_apps;
    GHashTable *pinned_launchers;
    GHashTable *fixed_launchers;
    GtkWidget *window;
    GdkRectangle monitor_geometry;
    guint resize_source;
} DockState;

static DockState dock_state;
static void free_window_data(gpointer data, GClosure *closure);
static Window find_running_app(const char *class_pattern);
static gboolean on_launcher_button_press(GtkWidget *button,
                                         GdkEventButton *event,
                                         gpointer user_data);

static gboolean resize_dock_to_contents(gpointer user_data) {
    (void)user_data;
    dock_state.resize_source = 0;
    if (!dock_state.window || !dock_state.dock_box) return G_SOURCE_REMOVE;

    gtk_widget_queue_resize(dock_state.dock_box);
    GtkRequisition minimum;
    GtkRequisition natural;
    gtk_widget_get_preferred_size(dock_state.dock_box, &minimum, &natural);
    (void)minimum;
    gtk_window_resize(GTK_WINDOW(dock_state.window), natural.width,
                      MAX(natural.height, 78));
    return G_SOURCE_REMOVE;
}

static void schedule_dock_resize(void) {
    if (!dock_state.resize_source) {
        dock_state.resize_source =
            g_idle_add(resize_dock_to_contents, NULL);
    }
}

static char* get_dock_asset_path(const char *relative_path) {
    static char path[512];
    snprintf(path, sizeof(path), "/usr/share/macos-de/%s", relative_path);
    struct stat buffer;
    if (stat(path, &buffer) == 0) return path;
    snprintf(path, sizeof(path), "%s", relative_path);
    return path;
}

static gboolean on_dock_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    return FALSE;
}

static void apply_rounded_window_shape(GtkWidget *widget, GtkAllocation *allocation, gpointer data) {
    if (widget == dock_state.window &&
        dock_state.monitor_geometry.width > 0) {
        gtk_window_move(GTK_WINDOW(widget),
            dock_state.monitor_geometry.x +
                (dock_state.monitor_geometry.width - allocation->width) / 2,
            dock_state.monitor_geometry.y +
                dock_state.monitor_geometry.height - allocation->height - 8);
    }
    GdkScreen *screen = gtk_widget_get_screen(widget);
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window || gdk_screen_is_composited(screen)) return;

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, allocation->width, allocation->height);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    const double radius = 20;
    cairo_new_sub_path(cr);
    cairo_arc(cr, allocation->width - radius, radius, radius, -G_PI_2, 0);
    cairo_arc(cr, allocation->width - radius, allocation->height - radius,
              radius, 0, G_PI_2);
    cairo_arc(cr, radius, allocation->height - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, radius, radius, radius, G_PI, 3 * G_PI_2);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_destroy(cr);

    cairo_region_t *shape = gdk_cairo_region_create_from_surface(surface);
    gdk_window_shape_combine_region(window, shape, 0, 0);
    cairo_region_destroy(shape);
    cairo_surface_destroy(surface);
}

static GtkWidget *create_dock_button(GtkWidget *icon, GtkWidget **indicator) {
    GtkWidget *button = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "dock-item");
    GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *marker = gtk_drawing_area_new();
    gtk_widget_set_size_request(marker, 12, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(marker),
                                "running-indicator");
    gtk_widget_set_no_show_all(marker, TRUE);
    gtk_box_pack_start(GTK_BOX(contents), icon, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(contents), marker, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(button), contents);
    *indicator = marker;
    return button;
}

static char *window_class_name(Display *display, Window window) {
    XClassHint hint = {0};
    if (!XGetClassHint(display, window, &hint)) return g_strdup("");
    char *class_name = g_ascii_strdown(hint.res_class ? hint.res_class :
                                      (hint.res_name ? hint.res_name : ""), -1);
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return class_name;
}

static int class_matches(const char *class_name, const char *app) {
    return strstr(class_name, app) != NULL;
}

static void activate_window(Window window) {
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
}

static GDesktopAppInfo *desktop_app_for_id(const char *app_id) {
    char *desktop_id = g_str_has_suffix(app_id, ".desktop")
        ? g_strdup(app_id) : g_strconcat(app_id, ".desktop", NULL);
    GDesktopAppInfo *app = g_desktop_app_info_new(desktop_id);
    g_free(desktop_id);
    return app;
}

static char *launcher_wm_class(const char *app_id, GDesktopAppInfo *app) {
    const char *wm_class = app
        ? g_desktop_app_info_get_startup_wm_class(app) : NULL;
    if (wm_class && *wm_class) return g_ascii_strdown(wm_class, -1);
    char *fallback = g_strdup(app_id);
    char *suffix = g_strrstr(fallback, ".desktop");
    if (suffix) *suffix = '\0';
    char *lower = g_ascii_strdown(fallback, -1);
    g_free(fallback);
    return lower;
}

static void free_pinned_launcher(gpointer data) {
    PinnedLauncher *launcher = data;
    if (launcher->button) gtk_widget_destroy(launcher->button);
    g_free(launcher->wm_class);
    g_free(launcher->icon_path);
    g_free(launcher);
}

static GdkPixbuf *load_launcher_icon(GKeyFile *settings, const char *app_id,
                                     GDesktopAppInfo *app) {
    char *custom_path = macos_de_dock_settings_get_icon(settings, app_id);
    if (custom_path) {
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
            custom_path, 48, 48, TRUE, NULL);
        if (pixbuf) {
            g_free(custom_path);
            return pixbuf;
        }
    }
    g_free(custom_path);

    if (app) {
        GIcon *icon = g_app_info_get_icon(G_APP_INFO(app));
        if (icon) {
            GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(
                gtk_icon_theme_get_default(), icon, 48,
                GTK_ICON_LOOKUP_FORCE_SIZE);
            if (info) {
                GdkPixbuf *pixbuf = gtk_icon_info_load_icon(info, NULL);
                g_object_unref(info);
                if (pixbuf) return pixbuf;
            }
        }
    }
    return NULL;
}

static void launch_pinned_app(const char *app_id) {
    GDesktopAppInfo *app = desktop_app_for_id(app_id);
    char *wm_class = launcher_wm_class(app_id, app);
    Window running = find_running_app(wm_class);
    if (running) {
        activate_window(running);
        g_free(wm_class);
        g_clear_object(&app);
        return;
    }

    cursor_system_start_animation(SYSTEM_CURSOR_WORKING, 1800);
    GError *error = NULL;
    gboolean launched = app &&
        g_app_info_launch(G_APP_INFO(app), NULL, NULL, &error);
    if (!launched && !error) {
        char *program = g_strdup(app_id);
        char *suffix = g_strrstr(program, ".desktop");
        if (suffix) *suffix = '\0';
        char *path = g_find_program_in_path(program);
        if (path) {
            char *argv[] = { path, NULL };
            launched = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                     NULL, NULL, NULL, &error);
            g_free(path);
        } else {
            g_warning("No application or executable was found for dock item %s.",
                      app_id);
        }
        g_free(program);
    }
    if (!launched && error) {
        g_warning("Failed to launch dock item %s: %s", app_id, error->message);
        g_clear_error(&error);
    }
    g_free(wm_class);
    g_clear_object(&app);
}

static void on_pinned_launcher_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    launch_pinned_app((const char *)user_data);
}

static GtkWidget *create_pinned_launcher_icon(GKeyFile *settings,
                                               const char *app_id,
                                               GDesktopAppInfo *app) {
    GdkPixbuf *pixbuf = load_launcher_icon(settings, app_id, app);
    GtkWidget *icon = pixbuf
        ? gtk_image_new_from_pixbuf(pixbuf)
        : gtk_image_new_from_icon_name("application-x-executable",
                                        GTK_ICON_SIZE_LARGE_TOOLBAR);
    if (pixbuf) g_object_unref(pixbuf);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    return icon;
}

static void update_pinned_launchers(GKeyFile *settings) {
    if (!dock_state.pinned_launchers) {
        dock_state.pinned_launchers = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, free_pinned_launcher);
    }

    GError *error = NULL;
    gsize count = 0;
    char **app_ids = g_key_file_get_keys(settings, "pinned-apps", &count, &error);
    if (error) {
        if (!g_error_matches(error, G_KEY_FILE_ERROR,
                             G_KEY_FILE_ERROR_GROUP_NOT_FOUND)) {
            g_warning("Could not read pinned dock applications: %s",
                      error->message);
        }
        g_clear_error(&error);
    }
    for (gsize i = 0; app_ids && i < count; i++) {
        const char *app_id = app_ids[i];
        if (!macos_de_dock_settings_is_pinned(settings, app_id)) continue;
        GDesktopAppInfo *app = desktop_app_for_id(app_id);
        PinnedLauncher *launcher =
            g_hash_table_lookup(dock_state.pinned_launchers, app_id);
        char *icon_path = macos_de_dock_settings_get_icon(settings, app_id);
        if (!launcher) {
            launcher = g_new0(PinnedLauncher, 1);
            launcher->button = gtk_button_new();
            launcher->icon = create_pinned_launcher_icon(settings, app_id, app);
            launcher->indicator = gtk_drawing_area_new();
            gtk_widget_set_size_request(launcher->indicator, 12, 2);
            gtk_style_context_add_class(
                gtk_widget_get_style_context(launcher->button), "dock-item");
            gtk_style_context_add_class(
                gtk_widget_get_style_context(launcher->indicator),
                "running-indicator");
            gtk_widget_set_no_show_all(launcher->indicator, TRUE);
            GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
            gtk_box_pack_start(GTK_BOX(contents), launcher->icon,
                               TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(contents), launcher->indicator,
                               FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(launcher->button), contents);
            const char *title = app
                ? g_app_info_get_display_name(G_APP_INFO(app)) : app_id;
            gtk_widget_set_tooltip_text(launcher->button, title);
            g_signal_connect_data(
                launcher->button, "clicked",
                G_CALLBACK(on_pinned_launcher_clicked), g_strdup(app_id),
                free_window_data, 0);
            gtk_box_pack_start(GTK_BOX(dock_state.dock_box), launcher->button,
                               FALSE, FALSE, 0);
            g_signal_connect_data(
                launcher->button, "button-press-event",
                G_CALLBACK(on_launcher_button_press), g_strdup(app_id),
                free_window_data, 0);
            launcher->wm_class = launcher_wm_class(app_id, app);
            launcher->icon_path = icon_path;
            icon_path = NULL;
            g_hash_table_insert(dock_state.pinned_launchers,
                                g_strdup(app_id), launcher);
            gtk_widget_show_all(launcher->button);
            gtk_widget_hide(launcher->indicator);
            schedule_dock_resize();
        } else if (g_strcmp0(launcher->icon_path, icon_path) != 0) {
            GdkPixbuf *pixbuf = load_launcher_icon(settings, app_id, app);
            if (pixbuf) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(launcher->icon), pixbuf);
                g_object_unref(pixbuf);
            }
            g_free(launcher->icon_path);
            launcher->icon_path = g_strdup(icon_path);
        }
        g_free(icon_path);
        g_clear_object(&app);
    }
    g_strfreev(app_ids);

    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, dock_state.pinned_launchers);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (!macos_de_dock_settings_is_pinned(settings, key)) {
            g_hash_table_iter_remove(&iter);
            schedule_dock_resize();
        }
    }
}

static void register_fixed_launcher(GtkWidget *button, const char *app_id) {
    g_object_set_data(G_OBJECT(button), "fixed-launcher", GINT_TO_POINTER(TRUE));
    g_signal_connect_data(
        button, "button-press-event",
        G_CALLBACK(on_launcher_button_press), g_strdup(app_id),
        free_window_data, 0);
    g_hash_table_insert(dock_state.fixed_launchers, g_strdup(app_id), button);
}

static void update_fixed_launcher_visibility(GKeyFile *settings) {
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&iter, dock_state.fixed_launchers);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gboolean hidden = g_key_file_get_boolean(
            settings, "hidden-launchers", key, NULL);
        gtk_widget_set_visible(GTK_WIDGET(value), !hidden);
    }
}

static Window find_running_app(const char *class_pattern) {
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Atom actual_type;
    int actual_format;
    unsigned long count, bytes_after;
    unsigned char *data = NULL;
    Window result = None;
    Atom clients_atom = XInternAtom(display, "_NET_CLIENT_LIST", False);
    if (XGetWindowProperty(display, DefaultRootWindow(display), clients_atom,
                           0, 4096, False, XA_WINDOW, &actual_type,
                           &actual_format, &count, &bytes_after, &data) == Success &&
        data && actual_type == XA_WINDOW && actual_format == 32) {
        Window *clients = (Window *)data;
        for (unsigned long i = 0; i < count; i++) {
            char *name = window_class_name(display, clients[i]);
            if (class_matches(name, class_pattern)) {
                result = clients[i];
                g_free(name);
                break;
            }
            g_free(name);
        }
    }
    if (data) XFree(data);
    return result;
}

static gboolean launch_first_available(const char *const *programs,
                                       const char *app_name) {
    for (guint i = 0; programs[i]; i++) {
        char *path = g_find_program_in_path(programs[i]);
        if (!path) continue;
        char *argv[] = {path, NULL};
        GError *error = NULL;
        gboolean launched = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                          NULL, NULL, NULL, &error);
        if (!launched) {
            g_warning("Failed to launch %s using %s: %s",
                      app_name, path, error->message);
            g_clear_error(&error);
        }
        g_free(path);
        return launched;
    }
    g_warning("No supported application was found for %s.", app_name);
    return FALSE;
}

static void on_finder_clicked(GtkButton *btn, gpointer data) {
    Window running = find_running_app("finder");
    if (running) {
        activate_window(running);
        return;
    }

    const char *candidates[] = {
        "/usr/share/macos-de/apps/finder/finder",
        "build/apps/finder/finder"
    };
    const char *finder_path = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
        if (g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE)) {
            finder_path = candidates[i];
            break;
        }
    }

    if (!finder_path) {
        g_warning("Finder executable was not found. Build the project or install macOS-DE.");
        return;
    }

    cursor_system_start_animation(SYSTEM_CURSOR_WORKING, 1800);
    char *argv[] = { (char *)finder_path, NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning("Failed to launch Finder: %s", error->message);
        g_clear_error(&error);
    }
}

static void on_terminal_clicked(GtkButton *btn, gpointer data) {
    Window running = find_running_app("easy-term");
    if (running) {
        activate_window(running);
        return;
    }
    cursor_system_start_animation(SYSTEM_CURSOR_WORKING, 1800);
    const char *candidates[] = {
        "/usr/share/macos-de/apps/Easy-Term/Easy-Term",
        "build/apps/Easy-Term/Easy-Term",
        NULL
    };
    const char *path = NULL;
    for (guint i = 0; candidates[i]; i++) {
        if (g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE)) {
            path = candidates[i];
            break;
        }
    }
    if (!path) {
        g_warning("Easy-Term was not found. Build the project or install macOS-DE.");
        return;
    }
    char *argv[] = { (char *)path, NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning("Could not start Easy-Term: %s", error->message);
        g_clear_error(&error);
    }
}

static void on_settings_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    Window running = find_running_app("de-settings");
    if (running) {
        activate_window(running);
        return;
    }
    cursor_system_start_animation(SYSTEM_CURSOR_WORKING, 1800);
    const char *candidates[] = {
        "/usr/share/macos-de/apps/DE-settings/DE-settings",
        "build/apps/DE-settings/DE-settings",
        NULL
    };
    const char *path = NULL;
    for (guint i = 0; candidates[i]; i++) {
        if (g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE)) {
            path = candidates[i];
            break;
        }
    }
    if (!path) {
        g_warning("DE Settings was not found. Build the project or install macOS-DE.");
        return;
    }
    char *argv[] = { (char *)path, NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning("Could not start DE Settings: %s", error->message);
        g_clear_error(&error);
    }
}

static void on_easy_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    Window running = find_running_app("easy-file");
    if (running) {
        activate_window(running);
        return;
    }
    cursor_system_start_animation(SYSTEM_CURSOR_WORKING, 1800);
    const char *candidates[] = {
        "/usr/share/macos-de/apps/Easy-File/Easy-File",
        "build/apps/Easy-File/Easy-File",
        NULL
    };
    const char *path = NULL;
    for (guint i = 0; candidates[i]; i++) {
        if (g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE)) {
            path = candidates[i];
            break;
        }
    }
    if (!path) {
        g_warning("Easy-File was not found. Build the project or install macOS-DE.");
        return;
    }
    char *argv[] = { (char *)path, NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning("Could not start Easy-File: %s", error->message);
        g_clear_error(&error);
    }
}

static int is_pinned_class(const char *class_name) {
    return class_matches(class_name, "finder") ||
           class_matches(class_name, "easy-file") ||
           class_matches(class_name, "easy-term") ||
           class_matches(class_name, "de-settings") ||
           class_matches(class_name, "xterm") ||
           class_matches(class_name, "gnome-terminal") ||
           class_matches(class_name, "xfce4-terminal") ||
           class_matches(class_name, "konsole") ||
           class_matches(class_name, "control-center") ||
           class_matches(class_name, "settings-manager");
}

static void activate_dynamic_app(GtkButton *button, gpointer user_data) {
    Window *window = user_data;
    activate_window(*window);
}

static void free_window_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

typedef struct {
    GtkWidget *button;
    char *app_id;
    gboolean fixed;
} DockLauncherRemoval;

static void free_launcher_removal(gpointer data, GClosure *closure) {
    (void)closure;
    DockLauncherRemoval *removal = data;
    g_free(removal->app_id);
    g_free(removal);
}

static void on_remove_dock_launcher(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    DockLauncherRemoval *removal = user_data;
    GKeyFile *settings = macos_de_dock_settings_load();
    if (removal->fixed) {
        g_key_file_set_boolean(settings, "hidden-launchers",
                               removal->app_id, TRUE);
        if (macos_de_dock_settings_save(settings))
            gtk_widget_hide(removal->button);
    } else {
        macos_de_dock_settings_set_pinned(settings, removal->app_id, FALSE);
        if (macos_de_dock_settings_save(settings))
            update_pinned_launchers(settings);
    }
    g_key_file_unref(settings);
}

static gboolean on_launcher_button_press(GtkWidget *button,
                                         GdkEventButton *event,
                                         gpointer user_data) {
    if (event->type != GDK_BUTTON_PRESS ||
        event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    DockLauncherRemoval *removal = g_new0(DockLauncherRemoval, 1);
    removal->button = button;
    removal->app_id = g_strdup(user_data);
    removal->fixed =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "fixed-launcher"));
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *remove = gtk_menu_item_new_with_label("Remove from Dock");
    g_signal_connect_data(remove, "activate",
                          G_CALLBACK(on_remove_dock_launcher), removal,
                          free_launcher_removal, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), remove);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    g_signal_connect_swapped(menu, "selection-done",
                             G_CALLBACK(gtk_widget_destroy), menu);
    return TRUE;
}

static void free_running_app(gpointer data) {
    RunningApp *app = data;
    g_free(app->class_name);
    g_free(app);
}

static void add_dynamic_app(Display *display, Window window, char *class_name) {
    char *title = NULL;
    XFetchName(display, window, &title);
    RunningApp *app = g_new0(RunningApp, 1);
    app->window = window;
    app->class_name = class_name;
    app->seen = TRUE;
    app->button = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(app->button),
                                "dock-item");

    GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *icon = gtk_image_new_from_icon_name(
        "application-x-executable", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 42);
    GtkWidget *indicator = gtk_drawing_area_new();
    gtk_widget_set_size_request(indicator, 12, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(indicator),
                                "running-indicator");
    gtk_box_pack_start(GTK_BOX(contents), icon, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(contents), indicator, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->button), contents);
    gtk_widget_set_tooltip_text(app->button,
                                title && *title ? title : app->class_name);
    app->window_data = g_new(Window, 1);
    *app->window_data = window;
    g_signal_connect_data(app->button, "clicked",
                          G_CALLBACK(activate_dynamic_app), app->window_data,
                          free_window_data, 0);
    gtk_box_pack_start(GTK_BOX(dock_state.dock_box), app->button,
                       FALSE, FALSE, 0);
    gtk_widget_show_all(app->button);
    g_ptr_array_add(dock_state.running_apps, app);
    schedule_dock_resize();
    if (title) XFree(title);
}

static gboolean update_running_apps(gpointer user_data) {
    (void)user_data;
    GKeyFile *settings = macos_de_dock_settings_load();
    update_pinned_launchers(settings);
    update_fixed_launcher_visibility(settings);
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Atom actual_type;
    int actual_format;
    unsigned long count, bytes_after;
    unsigned char *data = NULL;
    Atom clients_atom = XInternAtom(display, "_NET_CLIENT_LIST", False);
    if (XGetWindowProperty(display, DefaultRootWindow(display), clients_atom,
                           0, 4096, False, XA_WINDOW, &actual_type,
                           &actual_format, &count, &bytes_after, &data) != Success ||
        !data || actual_type != XA_WINDOW || actual_format != 32) {
        if (data) XFree(data);
        g_key_file_unref(settings);
        return G_SOURCE_CONTINUE;
    }

    int finder_running = 0, file_running = 0;
    int terminal_running = 0, settings_running = 0;
    GHashTableIter pinned_iter;
    gpointer pinned_value;
    g_hash_table_iter_init(&pinned_iter, dock_state.pinned_launchers);
    while (g_hash_table_iter_next(&pinned_iter, NULL, &pinned_value)) {
        PinnedLauncher *launcher = pinned_value;
        gtk_widget_hide(launcher->indicator);
    }

    Window *clients = (Window *)data;
    for (guint i = 0; i < dock_state.running_apps->len; i++) {
        RunningApp *app = g_ptr_array_index(dock_state.running_apps, i);
        app->seen = FALSE;
    }
    for (unsigned long i = 0; i < count; i++) {
        char *class_name = window_class_name(display, clients[i]);
        gboolean pinned_running = FALSE;
        g_hash_table_iter_init(&pinned_iter, dock_state.pinned_launchers);
        while (g_hash_table_iter_next(&pinned_iter, NULL, &pinned_value)) {
            PinnedLauncher *launcher = pinned_value;
            if (class_matches(class_name, launcher->wm_class)) {
                gtk_widget_show(launcher->indicator);
                pinned_running = TRUE;
            }
        }
        if (pinned_running) {
            if (class_matches(class_name, "easy-file")) file_running = 1;
            else if (class_matches(class_name, "easy-term")) terminal_running = 1;
            else if (class_matches(class_name, "de-settings")) settings_running = 1;
            g_free(class_name);
            continue;
        }
        if (class_matches(class_name, "finder")) finder_running = 1;
        else if (class_matches(class_name, "easy-file")) file_running = 1;
        else if (class_matches(class_name, "easy-term") ||
                 class_matches(class_name, "xterm") ||
                 class_matches(class_name, "gnome-terminal") ||
                 class_matches(class_name, "xfce4-terminal") ||
                 class_matches(class_name, "konsole")) terminal_running = 1;
        else if (class_matches(class_name, "de-settings") ||
                 class_matches(class_name, "control-center") ||
                 class_matches(class_name, "settings-manager")) settings_running = 1;
        else if (!is_pinned_class(class_name)) {
            RunningApp *app = NULL;
            for (guint j = 0; j < dock_state.running_apps->len; j++) {
                RunningApp *candidate =
                    g_ptr_array_index(dock_state.running_apps, j);
                if (g_str_equal(candidate->class_name, class_name)) {
                    app = candidate;
                    break;
                }
            }
            if (!app) {
                add_dynamic_app(display, clients[i], class_name);
            } else {
                app->window = clients[i];
                *app->window_data = clients[i];
                app->seen = TRUE;
                g_free(class_name);
            }
        } else {
            g_free(class_name);
        }
    }

    if (finder_running) gtk_widget_show(dock_state.finder_indicator);
    else gtk_widget_hide(dock_state.finder_indicator);
    if (file_running) gtk_widget_show(dock_state.file_indicator);
    else gtk_widget_hide(dock_state.file_indicator);
    if (terminal_running) gtk_widget_show(dock_state.terminal_indicator);
    else gtk_widget_hide(dock_state.terminal_indicator);
    if (settings_running) gtk_widget_show(dock_state.settings_indicator);
    else gtk_widget_hide(dock_state.settings_indicator);

    for (guint i = dock_state.running_apps->len; i > 0; i--) {
        RunningApp *app = g_ptr_array_index(dock_state.running_apps, i - 1);
        if (!app->seen) {
            gtk_widget_destroy(app->button);
            g_ptr_array_remove_index(dock_state.running_apps, i - 1);
            schedule_dock_resize();
        }
    }
    XFree(data);
    g_key_file_unref(settings);
    return G_SOURCE_CONTINUE;
}

void init_dock(void) {
    GtkWidget *dock_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    dock_state.window = dock_window;
    gtk_window_set_default_size(GTK_WINDOW(dock_window), -1, 78);
    gtk_window_set_title(GTK_WINDOW(dock_window), "macOS Dock");
    gtk_window_set_decorated(GTK_WINDOW(dock_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dock_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(dock_window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(dock_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(dock_window), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_widget_set_app_paintable(dock_window, TRUE);

    GdkScreen *screen = gtk_widget_get_screen(dock_window);
    GdkVisual *rgba_visual = gdk_screen_get_rgba_visual(screen);
    if (rgba_visual) {
        gtk_widget_set_visual(dock_window, rgba_visual);
    }
    if (!gdk_screen_is_composited(screen)) {
        g_warning("Dock transparency requires an active compositing window manager.");
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(dock_window), "macos-dock-window");
    g_signal_connect(dock_window, "draw", G_CALLBACK(on_dock_draw), NULL);
    g_signal_connect(dock_window, "size-allocate", G_CALLBACK(apply_rounded_window_shape), NULL);

    GtkWidget *dock_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    dock_state.dock_box = dock_box;
    dock_state.running_apps = g_ptr_array_new_with_free_func(free_running_app);
    dock_state.fixed_launchers = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(dock_box), "macos-dock");
    gtk_container_add(GTK_CONTAINER(dock_window), dock_box);

    // Finder
    char *finder_icon_path = get_dock_asset_path("assets/apps/finder.png");
    GError *error = NULL;
    GdkPixbuf *raw_pixbuf = gdk_pixbuf_new_from_file(finder_icon_path, &error);
    GtkWidget *icon_finder;
    
    if (!error && raw_pixbuf) {
        GdkPixbuf *scaled_pixbuf = gdk_pixbuf_scale_simple(raw_pixbuf, 48, 48, GDK_INTERP_BILINEAR);
        icon_finder = gtk_image_new_from_pixbuf(scaled_pixbuf);
        g_object_unref(raw_pixbuf);
        g_object_unref(scaled_pixbuf);
    } else {
        if (error) g_clear_error(&error);
        icon_finder = gtk_image_new_from_icon_name("system-file-manager", GTK_ICON_SIZE_LARGE_TOOLBAR);
    }

    GtkWidget *btn_finder = create_dock_button(icon_finder,
                                               &dock_state.finder_indicator);
    register_fixed_launcher(btn_finder, "finder");
    gtk_widget_set_tooltip_text(btn_finder, "Finder");
    g_signal_connect(btn_finder, "clicked", G_CALLBACK(on_finder_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(dock_box), btn_finder, FALSE, FALSE, 0);

    char *file_icon_path = get_dock_asset_path("apps/Easy-File/logo.png");
    GdkPixbuf *file_pixbuf = gdk_pixbuf_new_from_file_at_scale(
        file_icon_path, 48, 48, TRUE, NULL);
    GtkWidget *file_icon = file_pixbuf
        ? gtk_image_new_from_pixbuf(file_pixbuf)
        : gtk_image_new_from_icon_name("system-file-manager",
                                       GTK_ICON_SIZE_LARGE_TOOLBAR);
    if (file_pixbuf) g_object_unref(file_pixbuf);
    GtkWidget *btn_file = create_dock_button(
        file_icon, &dock_state.file_indicator);
    register_fixed_launcher(btn_file, "easy-file");
    gtk_widget_set_tooltip_text(btn_file, "Easy-File");
    g_signal_connect(btn_file, "clicked", G_CALLBACK(on_easy_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(dock_box), btn_file, FALSE, FALSE, 0);

    // Terminal
    char *terminal_icon_path = get_dock_asset_path("apps/Easy-Term/logo.png");
    GdkPixbuf *terminal_pixbuf = gdk_pixbuf_new_from_file_at_scale(
        terminal_icon_path, 48, 48, TRUE, NULL);
    GtkWidget *icon_term = terminal_pixbuf
        ? gtk_image_new_from_pixbuf(terminal_pixbuf)
        : gtk_image_new_from_icon_name("utilities-terminal",
                                       GTK_ICON_SIZE_LARGE_TOOLBAR);
    if (terminal_pixbuf) g_object_unref(terminal_pixbuf);
    gtk_widget_set_size_request(icon_term, 48, 48);
    GtkWidget *btn_term = create_dock_button(icon_term,
                                             &dock_state.terminal_indicator);
    register_fixed_launcher(btn_term, "easy-term");
    gtk_widget_set_tooltip_text(btn_term, "Easy-Term");
    g_signal_connect(btn_term, "clicked", G_CALLBACK(on_terminal_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(dock_box), btn_term, FALSE, FALSE, 0);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(separator), "dock-separator");
    gtk_widget_set_size_request(separator, 1, 34);
    gtk_widget_set_valign(separator, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(dock_box), separator, FALSE, FALSE, 2);

    // Settings
    char *settings_icon_path = get_dock_asset_path(
        "apps/DE-settings/logo.png");
    GdkPixbuf *settings_pixbuf = gdk_pixbuf_new_from_file_at_scale(
        settings_icon_path, 48, 48, TRUE, NULL);
    GtkWidget *icon_settings = settings_pixbuf
        ? gtk_image_new_from_pixbuf(settings_pixbuf)
        : gtk_image_new_from_icon_name("preferences-system",
                                       GTK_ICON_SIZE_LARGE_TOOLBAR);
    if (settings_pixbuf) g_object_unref(settings_pixbuf);
    gtk_widget_set_size_request(icon_settings, 48, 48);
    GtkWidget *btn_settings = create_dock_button(icon_settings,
                                                 &dock_state.settings_indicator);
    register_fixed_launcher(btn_settings, "de-settings");
    gtk_widget_set_tooltip_text(btn_settings, "Settings");
    g_signal_connect(btn_settings, "clicked", G_CALLBACK(on_settings_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(dock_box), btn_settings, FALSE, FALSE, 0);

    // Position slim dock at bottom center
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);

    gtk_widget_realize(dock_window);
    gtk_widget_show_all(dock_window);
    GKeyFile *dock_settings = macos_de_dock_settings_load();
    update_fixed_launcher_visibility(dock_settings);
    g_key_file_unref(dock_settings);
    cursor_system_track_widget(dock_window);

    gint dock_w, dock_h;
    gtk_window_get_size(GTK_WINDOW(dock_window), &dock_w, &dock_h);

    if (monitor) {
        GdkRectangle geom;
        gdk_monitor_get_geometry(monitor, &geom);
        dock_state.monitor_geometry = geom;
        gtk_window_move(GTK_WINDOW(dock_window), geom.x + (geom.width - dock_w) / 2, geom.y + geom.height - dock_h - 8);
    } else {
        gtk_window_set_position(GTK_WINDOW(dock_window), GTK_WIN_POS_CENTER);
    }
    g_timeout_add_seconds(1, update_running_apps, NULL);
}