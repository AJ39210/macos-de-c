#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangofc-fontmap.h>
#include <pango/pangocairo.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "wallpaper_engine/wallpaper.h"
#include "desktop_engine/desktop_icons.h"
#include "widget_engine/widget_manager.h"
#include "cursor_engine/cursor_animator.h"
#include "startup_engine/startup.h"
#include "dock/dock.h"
#include "misc/garbage_collector/garbage_collector.h"
#include "lib/system_status.h"
#include "lib/lock_screen.h"

static GSubprocess *compositor_process;
static MacosDeLockScreen *desktop_lock_screen;
static GDBusConnection *desktop_bus_connection;
static GDBusNodeInfo *desktop_bus_introspection;
static guint desktop_bus_registration;
static guint desktop_bus_name_owner;
#define DESKTOP_BUS_NAME "org.macosde.Desktop"
#define DESKTOP_BUS_OBJECT "/org/macosde/Desktop"
#define DESKTOP_BUS_INTERFACE "org.macosde.Desktop"
char *get_asset_path(const char *relative_path);

typedef enum {
    POWER_ACTION_LOGOUT,
    POWER_ACTION_REBOOT,
    POWER_ACTION_SHUTDOWN
} PowerAction;

typedef struct {
    GtkWidget *window;
    gint64 start_time;
} FadeInData;

static GtkWidget *active_app_label;
static gboolean desktop_dark_theme = TRUE;
#define DESKTOP_FADE_DURATION_US 400000

static const gchar desktop_bus_xml[] =
    "<node>"
    " <interface name='" DESKTOP_BUS_INTERFACE "'>"
    "  <method name='Lock'/>"
    " </interface>"
    "</node>";

static void
on_desktop_bus_method_call(GDBusConnection *connection,
                           const gchar *sender, const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name, GVariant *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;

    GError *error = NULL;
    if (g_str_equal(method_name, "Lock") &&
        macos_de_lock_screen_lock_now(desktop_lock_screen, &error)) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
        return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.macosde.Desktop.Error.LockUnavailable",
        error != NULL ? error->message :
        "The desktop lock screen could not be activated.");
    g_clear_error(&error);
}

static const GDBusInterfaceVTable desktop_bus_vtable = {
    .method_call = on_desktop_bus_method_call
};

static void
on_desktop_bus_name_lost(GDBusConnection *connection, const gchar *name,
                         gpointer user_data)
{
    (void)connection;
    (void)user_data;
    g_warning("Could not own session D-Bus name %s; remote desktop actions are unavailable.",
              name);
}

static void
start_desktop_bus_service(void)
{
    GError *error = NULL;
    desktop_bus_introspection = g_dbus_node_info_new_for_xml(
        desktop_bus_xml, &error);
    if (desktop_bus_introspection == NULL) {
        g_warning("Could not create desktop D-Bus interface: %s",
                  error->message);
        g_clear_error(&error);
        return;
    }

    desktop_bus_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (desktop_bus_connection == NULL) {
        g_warning("Could not connect the desktop to the session D-Bus: %s",
                  error->message);
        g_clear_error(&error);
        g_clear_pointer(&desktop_bus_introspection, g_dbus_node_info_unref);
        return;
    }

    desktop_bus_registration = g_dbus_connection_register_object(
        desktop_bus_connection, DESKTOP_BUS_OBJECT,
        desktop_bus_introspection->interfaces[0], &desktop_bus_vtable,
        NULL, NULL, &error);
    if (desktop_bus_registration == 0) {
        g_warning("Could not register desktop D-Bus actions: %s",
                  error->message);
        g_clear_error(&error);
        g_clear_object(&desktop_bus_connection);
        g_clear_pointer(&desktop_bus_introspection, g_dbus_node_info_unref);
        return;
    }
    desktop_bus_name_owner = g_bus_own_name_on_connection(
        desktop_bus_connection, DESKTOP_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
        NULL, on_desktop_bus_name_lost, NULL, NULL);
}

static void
stop_desktop_bus_service(void)
{
    if (desktop_bus_name_owner != 0)
        g_bus_unown_name(desktop_bus_name_owner);
    if (desktop_bus_connection != NULL && desktop_bus_registration != 0)
        g_dbus_connection_unregister_object(
            desktop_bus_connection, desktop_bus_registration);
    g_clear_object(&desktop_bus_connection);
    g_clear_pointer(&desktop_bus_introspection, g_dbus_node_info_unref);
    desktop_bus_name_owner = 0;
    desktop_bus_registration = 0;
}

typedef struct {
    GtkWidget *window;
    gchar *settings_path;
    gchar *default_path;
    gchar *applied_path;
    gchar *last_settings;
    guint source_id;
} WallpaperSettingsSync;

static void free_wallpaper_settings_sync(GtkWidget *window,
                                         gpointer user_data) {
    WallpaperSettingsSync *sync = user_data;
    (void)window;
    if (sync->source_id != 0)
        g_source_remove(sync->source_id);
    g_free(sync->settings_path);
    g_free(sync->default_path);
    g_free(sync->applied_path);
    g_free(sync->last_settings);
    g_free(sync);
}

static gboolean sync_wallpaper_settings(gpointer user_data) {
    WallpaperSettingsSync *sync = user_data;
    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(sync->settings_path, &contents, &length, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("Could not read desktop appearance settings: %s",
                      error->message);
        g_clear_error(&error);
        return G_SOURCE_CONTINUE;
    }
    if (g_strcmp0(contents, sync->last_settings) == 0) {
        g_free(contents);
        return G_SOURCE_CONTINUE;
    }
    g_free(sync->last_settings);
    sync->last_settings = g_strdup(contents);

    GKeyFile *settings = g_key_file_new();
    if (!g_key_file_load_from_data(settings, contents, length,
                                   G_KEY_FILE_NONE, &error)) {
        g_warning("Could not parse desktop appearance settings: %s",
                  error->message);
        g_clear_error(&error);
        g_key_file_unref(settings);
        g_free(contents);
        return G_SOURCE_CONTINUE;
    }
    g_free(contents);

    gchar *requested_path = g_key_file_get_string(
        settings, "Appearance", "wallpaper", NULL);
    g_key_file_unref(settings);
    const gchar *target_path = requested_path && *requested_path
        ? requested_path : sync->default_path;
    if (!g_file_test(target_path, G_FILE_TEST_IS_REGULAR)) {
        g_warning("The selected wallpaper does not exist: %s", target_path);
        g_free(requested_path);
        return G_SOURCE_CONTINUE;
    }
    if (g_strcmp0(target_path, sync->applied_path) != 0) {
        if (!wallpaper_window_set_image(sync->window, target_path, &error)) {
            g_warning("Could not apply wallpaper %s: %s", target_path,
                      error ? error->message : "unknown error");
            g_clear_error(&error);
        } else {
            g_free(sync->applied_path);
            sync->applied_path = g_strdup(target_path);
        }
    }
    g_free(requested_path);
    return G_SOURCE_CONTINUE;
}

static void watch_wallpaper_settings(GtkWidget *window,
                                     const gchar *initial_path) {
    WallpaperSettingsSync *sync = g_new0(WallpaperSettingsSync, 1);
    sync->window = window;
    sync->settings_path = g_build_filename(
        g_get_user_config_dir(), "macos-de", "settings.ini", NULL);
    sync->default_path = g_strdup(get_asset_path("assets/wallpaper/mac.png"));
    sync->applied_path = g_strdup(initial_path);
    g_signal_connect(window, "destroy",
                     G_CALLBACK(free_wallpaper_settings_sync), sync);
    sync->source_id = g_timeout_add_seconds(
        1, sync_wallpaper_settings, sync);
}

static void load_appearance_settings(char **theme, char **wallpaper) {
    *theme = g_strdup("dark");
    *wallpaper = NULL;
    char *settings_path = g_build_filename(
        g_get_user_config_dir(), "macos-de", "settings.ini", NULL);
    if (!g_file_test(settings_path, G_FILE_TEST_EXISTS)) {
        g_free(settings_path);
        return;
    }

    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(settings, settings_path,
                                   G_KEY_FILE_NONE, &error)) {
        g_warning("Could not load desktop settings from %s: %s",
                  settings_path, error->message);
        g_clear_error(&error);
        g_key_file_unref(settings);
        g_free(settings_path);
        return;
    }

    char *saved_theme = g_key_file_get_string(
        settings, "Appearance", "theme", NULL);
    if (saved_theme) {
        if (g_str_equal(saved_theme, "dark") ||
            g_str_equal(saved_theme, "light")) {
            g_free(*theme);
            *theme = saved_theme;
        } else {
            g_warning("Ignoring unsupported desktop theme '%s'", saved_theme);
            g_free(saved_theme);
        }
    }

    char *saved_wallpaper = g_key_file_get_string(
        settings, "Appearance", "wallpaper", NULL);
    if (saved_wallpaper && *saved_wallpaper &&
        g_file_test(saved_wallpaper, G_FILE_TEST_IS_REGULAR)) {
        *wallpaper = saved_wallpaper;
    } else {
        g_free(saved_wallpaper);
    }

    g_key_file_unref(settings);
    g_free(settings_path);
}

static gboolean draw_topbar_background(GtkWidget *widget, cairo_t *context,
                                       gpointer user_data) {
    (void)user_data;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    if (desktop_dark_theme) {
        cairo_set_source_rgba(context, 0.11, 0.11, 0.13, 0.94);
    } else {
        cairo_set_source_rgba(context, 0.96, 0.96, 0.97, 0.96);
    }
    cairo_paint(context);
    if (desktop_dark_theme) {
        cairo_set_source_rgba(context, 1, 1, 1, 0.12);
    } else {
        cairo_set_source_rgba(context, 0.08, 0.08, 0.11, 0.12);
    }
    cairo_set_line_width(context, 1);
    cairo_move_to(context, 0, allocation.height - 0.5);
    cairo_line_to(context, allocation.width, allocation.height - 0.5);
    cairo_stroke(context);
    return FALSE;
}

static gboolean update_desktop_heartbeat(gpointer user_data) {
    (void)user_data;
    const char *path = g_getenv("MACOS_DE_HEARTBEAT");
    if (path) {
        GError *error = NULL;
        if (!g_file_set_contents(path, "ok", 2, &error)) {
            g_warning("Could not update desktop heartbeat at %s: %s",
                      path, error->message);
            g_clear_error(&error);
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean update_clock(gpointer user_data) {
    GtkLabel *clock_label = GTK_LABEL(user_data);
    GDateTime *now = g_date_time_new_now_local();
    char *text = g_date_time_format(now, "%H:%M:%S");
    gtk_label_set_text(clock_label, text);
    g_free(text);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static gboolean quit_for_session_signal(gpointer user_data) {
    (void)user_data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void on_power_command_finished(GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data) {
    GError *error = NULL;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source_object), result, &error)) {
        g_warning("Power action failed: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(source_object);
}

static void perform_power_action(PowerAction action) {
    if (action == POWER_ACTION_LOGOUT) {
        gtk_main_quit();
        return;
    }

    const char *systemctl_action =
        action == POWER_ACTION_REBOOT ? "reboot" : "poweroff";
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, &error, "systemctl", systemctl_action, NULL);
    if (!process) {
        g_warning("Could not start systemctl %s: %s",
                  systemctl_action, error->message);
        g_clear_error(&error);
        return;
    }
    g_subprocess_wait_check_async(process, NULL, on_power_command_finished, NULL);
}

static char *get_desktop_log_path(void) {
    const char *configured_path = g_getenv("MACOS_DE_LOG_FILE");
    if (configured_path && *configured_path) {
        char *path = g_strdup(configured_path);
        char *directory = g_path_get_dirname(path);
        if (g_mkdir_with_parents(directory, 0755) == 0) {
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                          0644);
            if (fd >= 0) {
                close(fd);
                g_free(directory);
                return path;
            }
        }
        int log_error = errno;
        g_warning("Could not open the log location in %s: %s; using the user log directory.",
                  directory, g_strerror(log_error));
        g_free(directory);
        g_free(path);
    }

    char *system_log = g_strdup("/var/logs/MACOS-DE-LOGS.txt");
    char *system_log_directory = g_path_get_dirname(system_log);
    if (g_mkdir_with_parents(system_log_directory, 0755) == 0 &&
        access(system_log_directory, W_OK) == 0) {
        g_free(system_log_directory);
        return system_log;
    }
    g_free(system_log_directory);
    g_free(system_log);

    char *state_directory = g_build_filename(
        g_get_user_state_dir(), "macos-de", NULL);
    if (g_mkdir_with_parents(state_directory, 0700) != 0) {
        g_warning("Could not create user log directory %s.", state_directory);
    }
    char *path = g_build_filename(state_directory, "MACOS-DE-LOGS.txt", NULL);
    g_free(state_directory);
    return path;
}

static void append_desktop_log(const char *path, const char *message) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        g_warning("Could not append to desktop log %s: %s",
                  path, g_strerror(errno));
        return;
    }
    GDateTime *now = g_date_time_new_now_local();
    char *timestamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S %z");
    char *entry = g_strdup_printf("[%s] FATAL: %s\n", timestamp, message);
    ssize_t remaining = (ssize_t)strlen(entry);
    const char *cursor = entry;
    while (remaining > 0) {
        ssize_t written = write(fd, cursor, (size_t)remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            g_warning("Could not write desktop log %s: %s",
                      path, g_strerror(errno));
            break;
        }
        cursor += written;
        remaining -= written;
    }
    close(fd);
    g_free(entry);
    g_free(timestamp);
    g_date_time_unref(now);
}

static char *read_desktop_log_tail(const char *path) {
    enum { LOG_TAIL_LIMIT = 2800 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return g_strdup("No log output is available yet.");
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        close(fd);
        return g_strdup("Could not read the desktop log.");
    }
    off_t offset = MAX((off_t)0, file_size - LOG_TAIL_LIMIT);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        close(fd);
        return g_strdup("Could not seek to the end of the desktop log.");
    }
    gsize capacity = (gsize)(file_size - offset);
    char *buffer = g_malloc(capacity + 1);
    gsize length = 0;
    while (length < capacity) {
        ssize_t count = read(fd, buffer + length, capacity - length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        length += (gsize)count;
    }
    close(fd);
    buffer[length] = '\0';
    char *valid_text = g_utf8_make_valid(buffer, (gssize)length);
    g_free(buffer);
    return valid_text;
}

static gboolean on_startup_error_key_press(GtkWidget *widget,
                                           GdkEventKey *event,
                                           gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (event->keyval != GDK_KEY_Escape) return FALSE;
    perform_power_action(POWER_ACTION_LOGOUT);
    return TRUE;
}

static void show_startup_error(const char *message) {
    const char *reason = message && *message
        ? message : "The desktop environment could not be started.";
    char *log_path = get_desktop_log_path();
    append_desktop_log(log_path, reason);
    char *log_tail = read_desktop_log_tail(log_path);
    char *details = g_strdup_printf(
        "%s\n\nRecent log output:\n%s\nFull log: %s",
        reason, log_tail, log_path);
    g_free(log_tail);

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = display ? gdk_display_get_primary_monitor(display) : NULL;
    if (display && !monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle geometry = {0, 0, 1280, 720};
    if (monitor) gdk_monitor_get_geometry(monitor, &geometry);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "macOS-DE Startup Error");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), geometry.width, geometry.height);
    gtk_window_move(GTK_WINDOW(window), geometry.x, geometry.y);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_widget_add_events(window, GDK_KEY_PRESS_MASK);
    g_signal_connect(window, "key-press-event",
                     G_CALLBACK(on_startup_error_key_press), NULL);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(window), overlay);
    char *image_path = g_strdup(
        get_asset_path("assets/system/start-up/error_screen.png"));
    GError *image_error = NULL;
    GdkPixbuf *background = gdk_pixbuf_new_from_file_at_scale(
        image_path, geometry.width, geometry.height, TRUE, &image_error);
    if (background) {
        GtkWidget *image = gtk_image_new_from_pixbuf(background);
        gtk_container_add(GTK_CONTAINER(overlay), image);
        g_object_unref(background);
    } else {
        g_warning("Could not load startup error artwork %s: %s",
                  image_path, image_error ? image_error->message :
                  "unknown image loading error");
        g_clear_error(&image_error);
    }

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_widget_set_valign(text, GTK_ALIGN_START);
    gtk_widget_set_margin_start(text, MAX(24, geometry.width * 50 / 1920));
    gtk_widget_set_margin_top(text, MAX(72, geometry.height * 86 / 1080));
    gtk_widget_set_size_request(text, MAX(280, geometry.width * 9 / 10), -1);

    GtkWidget *details_label = gtk_label_new(details);
    gtk_label_set_xalign(GTK_LABEL(details_label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(details_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(details_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(details_label), TRUE);
    gtk_widget_set_name(details_label, "startup-error-details");
    GtkWidget *hint = gtk_label_new("Press ESC to return to the login screen.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_set_name(hint, "startup-error-hint");
    gtk_box_pack_start(GTK_BOX(text), details_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), hint, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), text);

    GtkCssProvider *provider = gtk_css_provider_new();
    static const char error_css[] =
        "#startup-error-details { color: #f5f5f7; font-family: monospace;"
        " font-size: 12px; }"
        "#startup-error-hint { color: #ff4242; font-size: 14px;"
        " font-weight: bold; }";
    GError *css_error = NULL;
    if (!gtk_css_provider_load_from_data(provider, error_css, -1, &css_error)) {
        g_warning("Could not style the startup error screen: %s",
                  css_error->message);
        g_clear_error(&css_error);
    } else {
        gtk_style_context_add_provider_for_screen(
            gtk_widget_get_screen(window), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);

    g_free(details);
    g_free(image_path);
    g_free(log_path);
    gtk_widget_show_all(window);
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(window);
}

static void on_logout_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    perform_power_action(POWER_ACTION_LOGOUT);
}

static void on_reboot_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    perform_power_action(POWER_ACTION_REBOOT);
}

static void on_shutdown_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    perform_power_action(POWER_ACTION_SHUTDOWN);
}

static void set_top_panel_strut(GtkWidget *window, const GdkRectangle *geometry,
                                int panel_height) {
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    if (!gdk_window) return;

    gulong strut[] = {
        0, 0, (gulong)(geometry->y + panel_height), 0,
        0, 0, 0, 0,
        (gulong)MAX(0, geometry->x),
        (gulong)MAX(0, geometry->x + geometry->width - 1),
        0, 0
    };
    gdk_property_change(
        gdk_window,
        gdk_atom_intern_static_string("_NET_WM_STRUT_PARTIAL"),
        gdk_atom_intern_static_string("CARDINAL"),
        32, GDK_PROP_MODE_REPLACE, (const guchar *)strut,
        (gint)G_N_ELEMENTS(strut));
}

static gboolean update_active_app_title(gpointer user_data) {
    GtkLabel *label = GTK_LABEL(user_data);
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Window root = DefaultRootWindow(display);
    Atom active_atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    Window active = None;
    if (XGetWindowProperty(display, root, active_atom, 0, 1, False, XA_WINDOW,
                           &actual_type, &actual_format, &count, &bytes_after,
                           &value) == Success &&
        value && actual_type == XA_WINDOW && actual_format == 32 && count) {
        active = *(Window *)value;
    }
    if (value) XFree(value);

    char *title = NULL;
    if (active != None) {
        Atom name_atom = XInternAtom(display, "_NET_WM_NAME", False);
        Atom utf8_atom = XInternAtom(display, "UTF8_STRING", False);
        if (XGetWindowProperty(display, active, name_atom, 0, 256, False,
                               utf8_atom, &actual_type, &actual_format, &count,
                               &bytes_after, &value) == Success &&
            value && actual_type == utf8_atom && actual_format == 8) {
            title = g_strndup((const char *)value, count);
        }
        if (value) XFree(value);
    }
    if (!title || !*title) {
        g_free(title);
        title = g_strdup("Desktop");
    }
    gtk_label_set_text(label, title);
    g_free(title);
    return G_SOURCE_CONTINUE;
}

static void register_sf_font(void) {
    char *font_path = get_asset_path(
        "assets/system/fonts/SF/SFPRODISPLAYBLACKITALIC.OTF");
    FcConfig *config = FcConfigGetCurrent();
    if (access(font_path, R_OK) == 0 &&
        FcConfigAppFontAddFile(config, (const FcChar8 *)font_path)) {
        FcConfigBuildFonts(config);
        PangoFontMap *font_map = pango_cairo_font_map_get_default();
        if (PANGO_IS_FC_FONT_MAP(font_map)) {
            pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(font_map));
        }
    } else {
        g_warning("Could not register the bundled SF Pro Display font at %s.",
                  font_path);
    }
}

static gboolean run_fade_in(gpointer user_data) {
    FadeInData *data = (FadeInData *)user_data;
    double opacity = (double)(g_get_monotonic_time() - data->start_time) /
                     DESKTOP_FADE_DURATION_US;
    if (opacity >= 1.0) {
        gtk_widget_set_opacity(data->window, 1.0);
        g_free(data);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_set_opacity(data->window, opacity);
    return G_SOURCE_CONTINUE;
}

void fade_in_window(GtkWidget *window) {
    gtk_widget_set_opacity(window, 0.0);
    gtk_widget_show_all(window);
    FadeInData *data = g_new0(FadeInData, 1);
    data->window = window;
    data->start_time = g_get_monotonic_time();
    g_timeout_add(16, run_fade_in, data);
}

char* get_asset_path(const char *relative_path) {
    static char path[512];
    snprintf(path, sizeof(path), "/usr/share/macos-de/%s", relative_path);
    struct stat buffer;
    if (stat(path, &buffer) == 0) return path;
    snprintf(path, sizeof(path), "%s", relative_path);
    return path;
}

static void start_compositor_if_needed(void) {
    GdkScreen *screen = gdk_screen_get_default();
    if (gdk_screen_is_composited(screen)) return;

    char *compositor_path = g_find_program_in_path("picom");
    if (!compositor_path) {
        g_warning("No compositor is active and picom was not found; install picom for transparency and fullscreen-game unredirect support.");
        return;
    }

    GError *error = NULL;
    compositor_process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, &error, compositor_path,
        "--unredir-if-possible", NULL);
    g_free(compositor_path);

    if (!compositor_process) {
        g_warning("Could not start picom: %s", error->message);
        g_clear_error(&error);
    }
}

static void stop_compositor(void) {
    if (!compositor_process) return;

    g_subprocess_send_signal(compositor_process, SIGTERM);
    GError *error = NULL;
    if (!g_subprocess_wait(compositor_process, NULL, &error)) {
        g_warning("Could not stop picom cleanly: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(compositor_process);
    compositor_process = NULL;
}

void launch_desktop_environment(gboolean success, const char *error_message,
                                gpointer user_data) {
    (void)user_data;
    if (!success) {
        show_startup_error(error_message);
        return;
    }

    register_sf_font();
    char *theme = NULL;
    char *wallpaper_path = NULL;
    load_appearance_settings(&theme, &wallpaper_path);
    gboolean dark_theme = g_str_equal(theme, "dark");
    desktop_dark_theme = dark_theme;

    // Load Main CSS (and dock CSS explicitly if separate)
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, get_asset_path("css/style.css"), NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);

    // Load dock.css styling
    GtkCssProvider *dock_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(dock_provider, get_asset_path("css/dock.css"), NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(dock_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(dock_provider);

    if (dark_theme) {
        static const char dark_theme_css[] =
            "window.system-status-panel {"
            " background-color: rgba(39, 40, 46, 0.96);"
            " color: #f5f5f7;"
            " border-color: rgba(255, 255, 255, 0.14);"
            "}"
            "window.system-status-panel scale trough {"
            " background-color: #55565f;"
            "}"
            "button.control-power-action {"
            " color: #f5f5f7;"
            " background-color: #45464e;"
            "}"
            "button.control-power-action:hover {"
            " background-color: #55565f;"
            "}";
        GtkCssProvider *theme_provider = gtk_css_provider_new();
        GError *error = NULL;
        if (!gtk_css_provider_load_from_data(
                theme_provider, dark_theme_css, -1, &error)) {
            g_warning("Could not apply desktop dark theme: %s",
                      error->message);
            g_clear_error(&error);
        } else {
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(), GTK_STYLE_PROVIDER(theme_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        }
        g_object_unref(theme_provider);
    } else {
        static const char light_theme_css[] =
            "window.macos-topbar { color: #24242a; }"
            "label.topbar-clock, label.topbar-app-title { color: #24242a; }"
            "button.power-menu-trigger:hover,"
            "button.power-menu-trigger:checked,"
            "button.topbar-status-button:hover {"
            " background-color: rgba(0, 0, 0, 0.08);"
            "}";
        GtkCssProvider *theme_provider = gtk_css_provider_new();
        GError *error = NULL;
        if (!gtk_css_provider_load_from_data(
                theme_provider, light_theme_css, -1, &error)) {
            g_warning("Could not apply desktop light theme: %s",
                      error->message);
            g_clear_error(&error);
        } else {
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(), GTK_STYLE_PROVIDER(theme_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        }
        g_object_unref(theme_provider);
    }

    // 1. Wallpaper Window with Fade-In
    if (!wallpaper_path) {
        wallpaper_path = g_strdup(get_asset_path("assets/wallpaper/mac.png"));
    }
    if (!g_file_test(wallpaper_path, G_FILE_TEST_IS_REGULAR)) {
        char *failure_message = g_strdup_printf(
            "The selected wallpaper could not be found: %s", wallpaper_path);
        g_free(wallpaper_path);
        g_free(theme);
        show_startup_error(failure_message);
        g_free(failure_message);
        return;
    }
    GError *wallpaper_error = NULL;
    GtkWidget *wallpaper_win = create_wallpaper_window(wallpaper_path,
                                                        &wallpaper_error);
    if (!wallpaper_win) {
        char *failure_message = g_strdup_printf(
            "Could not load wallpaper %s: %s", wallpaper_path,
            wallpaper_error ? wallpaper_error->message :
            "unknown image loading error");
        g_clear_error(&wallpaper_error);
        g_free(wallpaper_path);
        g_free(theme);
        show_startup_error(failure_message);
        g_free(failure_message);
        return;
    }
    watch_wallpaper_settings(wallpaper_win, wallpaper_path);
    GtkWidget *wallpaper_overlay =
        gtk_bin_get_child(GTK_BIN(wallpaper_win));
    gtk_overlay_add_overlay(GTK_OVERLAY(wallpaper_overlay),
                            macos_de_desktop_icons_create());
    gtk_overlay_add_overlay(GTK_OVERLAY(wallpaper_overlay),
                            macos_de_widget_manager_create());
    g_free(wallpaper_path);
    g_free(theme);
    fade_in_window(wallpaper_win);

    // 2. Top Bar Window with Fade-In
    GtkWidget *topbar_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(topbar_win), "macOS DE - Top Bar");
    gtk_window_set_decorated(GTK_WINDOW(topbar_win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(topbar_win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(topbar_win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(topbar_win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(topbar_win), GDK_WINDOW_TYPE_HINT_DOCK);
    GdkScreen *topbar_screen = gtk_widget_get_screen(topbar_win);
    GdkVisual *topbar_visual = gdk_screen_get_rgba_visual(topbar_screen);
    if (topbar_visual) {
        gtk_widget_set_app_paintable(topbar_win, TRUE);
        gtk_widget_set_visual(topbar_win, topbar_visual);
        g_signal_connect(topbar_win, "draw",
                         G_CALLBACK(draw_topbar_background), NULL);
    }
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle geometry = {0, 0, 1280, 800};
    if (monitor) gdk_monitor_get_geometry(monitor, &geometry);
    const int topbar_height = 30;
    gtk_window_set_default_size(GTK_WINDOW(topbar_win), geometry.width, topbar_height);
    gtk_window_move(GTK_WINDOW(topbar_win), geometry.x, geometry.y);

    GtkWidget *topbar_contents = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(topbar_win), topbar_contents);
    GtkWidget *clock_label = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(clock_label),
                                "topbar-clock");
    gtk_widget_set_margin_start(clock_label, 12);
    update_clock(clock_label);
    g_timeout_add_seconds(1, update_clock, clock_label);
    gtk_box_pack_start(GTK_BOX(topbar_contents), clock_label, FALSE, FALSE, 0);
    active_app_label = gtk_label_new("Desktop");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(active_app_label), "topbar-app-title");
    gtk_widget_set_margin_start(active_app_label, 10);
    gtk_label_set_ellipsize(GTK_LABEL(active_app_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(active_app_label), 28);
    update_active_app_title(active_app_label);
    g_timeout_add_seconds(1, update_active_app_title, active_app_label);
    gtk_box_pack_start(GTK_BOX(topbar_contents), active_app_label,
                       FALSE, FALSE, 0);
    GtkWidget *topbar_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(topbar_contents), topbar_spacer, TRUE, TRUE, 0);
    GtkWidget *power_button = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(power_button),
                                "power-menu-trigger");
    gtk_widget_set_tooltip_text(power_button, "Session and power options");
    GdkPixbuf *brand_icon = gdk_pixbuf_new_from_file_at_scale(
        get_asset_path("assets/system/icon.png"), 20, 20, TRUE, NULL);
    if (brand_icon) {
        gtk_button_set_image(GTK_BUTTON(power_button),
                             gtk_image_new_from_pixbuf(brand_icon));
        g_object_unref(brand_icon);
    } else {
        gtk_button_set_image(GTK_BUTTON(power_button),
                             gtk_image_new_from_icon_name("system-shutdown",
                                                          GTK_ICON_SIZE_MENU));
    }
    GtkWidget *status_icons = system_status_create(
        power_button, G_CALLBACK(on_logout_button_clicked),
        G_CALLBACK(on_reboot_button_clicked),
        G_CALLBACK(on_shutdown_button_clicked),
        get_asset_path("assets/system"), dark_theme);
    GtkWidget *topbar_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(topbar_right), status_icons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(topbar_right), power_button, FALSE, FALSE, 8);
    gtk_box_pack_end(GTK_BOX(topbar_contents), topbar_right, FALSE, FALSE, 0);

    GtkStyleContext *context = gtk_widget_get_style_context(topbar_win);
    gtk_style_context_add_class(context, "macos-topbar");

    gtk_widget_set_opacity(topbar_win, 0.0);
    gtk_widget_show_all(topbar_win);
    set_top_panel_strut(topbar_win, &geometry, topbar_height);
    g_signal_connect(topbar_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    FadeInData *topbar_fade = g_new0(FadeInData, 1);
    topbar_fade->window = topbar_win;
    topbar_fade->start_time = g_get_monotonic_time();
    g_timeout_add(16, run_fade_in, topbar_fade);

    // 3. Initialize the cursor pack before other shell windows are created.
    char *cursor_path = get_asset_path("assets/system/cursor/default1");
    init_cursor_system(topbar_win, cursor_path);
    cursor_system_track_widget(wallpaper_win);

    // 4. Initialize Floating macOS Dock.
    init_dock();
    desktop_lock_screen = macos_de_lock_screen_new(
        topbar_win, power_button, clock_label, active_app_label, status_icons,
        get_asset_path("assets/wallpaper/mac.png"));
    start_desktop_bus_service();
    GError *lock_error = NULL;
    if (macos_de_lock_screen_should_lock_on_startup(desktop_lock_screen) &&
        !macos_de_lock_screen_lock_now(desktop_lock_screen, &lock_error)) {
        const char *message = lock_error != NULL
            ? lock_error->message
            : "The desktop could not be secured after startup.";
        g_warning("Could not lock the desktop after startup: %s", message);
        show_startup_error(message);
        g_clear_error(&lock_error);
    }
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    if (argc >= 2 && g_str_equal(argv[1], "--error")) {
        show_startup_error(argc >= 3 ? argv[2] :
                           "The desktop environment exited unexpectedly.");
        gtk_main();
        return 0;
    }
    start_compositor_if_needed();
    garbage_collector_start();
    g_unix_signal_add(SIGTERM, quit_for_session_signal, NULL);
    g_unix_signal_add(SIGINT, quit_for_session_signal, NULL);
    g_timeout_add_seconds(2, update_desktop_heartbeat, NULL);
    update_desktop_heartbeat(NULL);

    // Kick off the cinematic boot sequence
    char *startup_dir = get_asset_path("assets/system/start-up");
    start_system_boot(startup_dir, launch_desktop_environment, NULL);

    gtk_main();
    stop_desktop_bus_service();
    macos_de_lock_screen_free(desktop_lock_screen);
    desktop_lock_screen = NULL;
    cleanup_cursor_system();
    stop_compositor();
    return 0;
}