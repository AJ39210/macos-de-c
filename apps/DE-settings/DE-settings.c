#include <gtk/gtk.h>
#include <cairo.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <spawn.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <string.h>
#include <glib/gstdio.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    GtkWidget *window;
    GtkWidget *theme_switch;
    GtkWidget *wallpaper_image;
    GtkWidget *wallpaper_status;
    GtkWidget *lock_wallpaper_image;
    GtkWidget *lock_wallpaper_status;
    GtkWidget *widget_status;
    GtkWidget *stats_switch;
    GtkWidget *button_shape;
    GtkWidget *window_corners;
    GtkWidget *lock_timeout;
    GKeyFile *settings;
    gchar *settings_path;
    gchar *executable_path;
    gboolean updating_switches;
    gboolean dark_theme;
} SettingsApp;

static gchar *requested_settings_page;

static gboolean validate_widget_zip(const gchar *path, GError **error);
static void refresh_widget_status(SettingsApp *app);

static gboolean
current_user_disables_automatic_lock(void)
{
    struct passwd *account = getpwuid(getuid());
    return account != NULL &&
        (g_ascii_strcasecmp(account->pw_name, "livesession") == 0 ||
         g_ascii_strcasecmp(account->pw_name, "default") == 0 ||
         g_ascii_strcasecmp(account->pw_name, "system") == 0);
}

typedef struct {
    double angle;
} CubeAnimation;

static gchar *
find_asset(const gchar *name, const gchar *argv0)
{
    gchar *executable_dir = g_path_get_dirname(argv0);
    gchar *candidate = g_build_filename(executable_dir, name, NULL);

    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
        g_free(executable_dir);
        return candidate;
    }

    g_free(candidate);
    g_free(executable_dir);
    if (g_file_test(name, G_FILE_TEST_IS_REGULAR))
        return g_strdup(name);
    return NULL;
}

static gchar *
get_settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "macos-de", "settings.ini", NULL);
}

static gboolean
save_settings(SettingsApp *app, GError **error)
{
    gchar *directory = g_path_get_dirname(app->settings_path);
    gchar *data;
    gsize length = 0;
    gboolean saved;

    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not create settings directory %s: %s",
                    directory, g_strerror(errno));
        g_free(directory);
        return FALSE;
    }

    data = g_key_file_to_data(app->settings, &length, error);
    g_free(directory);
    if (data == NULL)
        return FALSE;

    saved = g_file_set_contents(app->settings_path, data, (gssize) length, error);
    g_free(data);
    return saved;
}

static void
show_error(SettingsApp *app, const gchar *title, const gchar *message)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title);

    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static GtkWidget *
make_card(const gchar *title, const gchar *subtitle)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *heading = gtk_label_new(title);
    GtkWidget *description = gtk_label_new(subtitle);

    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "settings-card");
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "card-title");
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "muted");
    gtk_box_pack_start(GTK_BOX(frame), heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(frame), description, FALSE, FALSE, 0);
    return frame;
}

static GtkWidget *
make_page(guint spacing)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, (gint) spacing);
    gtk_container_set_border_width(GTK_CONTAINER(page), 22);
    gtk_widget_set_vexpand(page, TRUE);
    return page;
}

static void
add_setting_row(GtkWidget *card, const gchar *title, const gchar *description,
                GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *name = gtk_label_new(title);
    GtkWidget *detail = gtk_label_new(description);

    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_widget_set_halign(detail, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(detail), TRUE);
    gtk_widget_set_hexpand(labels, TRUE);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(name), "setting-title");
    gtk_style_context_add_class(gtk_widget_get_style_context(detail), "muted");
    gtk_box_pack_start(GTK_BOX(labels), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
}

static void
apply_theme(SettingsApp *app)
{
    GtkStyleContext *context = gtk_widget_get_style_context(app->window);
    gtk_style_context_remove_class(context, app->dark_theme ? "light" : "dark");
    gtk_style_context_add_class(context, app->dark_theme ? "dark" : "light");
}

static void
on_theme_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    SettingsApp *app = user_data;
    gboolean requested_dark;
    gboolean previous;
    GError *error = NULL;
    (void) property;

    if (app->updating_switches)
        return;

    requested_dark = gtk_switch_get_active(GTK_SWITCH(object));
    previous = app->dark_theme;
    g_key_file_set_string(app->settings, "Appearance", "theme",
                          requested_dark ? "dark" : "light");
    if (!save_settings(app, &error)) {
        g_key_file_set_string(app->settings, "Appearance", "theme",
                              previous ? "dark" : "light");
        app->updating_switches = TRUE;
        gtk_switch_set_active(GTK_SWITCH(app->theme_switch), previous);
        app->updating_switches = FALSE;
        show_error(app, "Theme was not saved", error->message);
        g_clear_error(&error);
        return;
    }

    app->dark_theme = requested_dark;
    apply_theme(app);
}

static void
on_stats_widget_changed(GObject *object, GParamSpec *property,
                        gpointer user_data)
{
    SettingsApp *app = user_data;
    gboolean requested = gtk_switch_get_active(GTK_SWITCH(object));
    gboolean had_value = g_key_file_has_key(
        app->settings, "Widgets", "stats_enabled", NULL);
    gboolean previous = g_key_file_get_boolean(
        app->settings, "Widgets", "stats_enabled", NULL);
    GError *error = NULL;
    (void)property;

    if (app->updating_switches)
        return;
    g_key_file_set_boolean(app->settings, "Widgets", "stats_enabled",
                           requested);
    if (save_settings(app, &error))
        return;

    if (had_value)
        g_key_file_set_boolean(app->settings, "Widgets", "stats_enabled",
                               previous);
    else
        g_key_file_remove_key(app->settings, "Widgets", "stats_enabled", NULL);
    app->updating_switches = TRUE;
    gtk_switch_set_active(GTK_SWITCH(app->stats_switch), previous);
    app->updating_switches = FALSE;
    show_error(app, "Widget setting was not saved",
               error ? error->message : "Settings could not be written.");
    g_clear_error(&error);
}

static void
on_window_manager_preference_changed(GtkComboBox *combo, gpointer user_data)
{
    SettingsApp *app = user_data;
    const gchar *key = g_object_get_data(G_OBJECT(combo), "settings-key");
    const gchar *value = gtk_combo_box_get_active_id(combo);
    if (key == NULL || value == NULL)
        return;

    gchar *previous = g_key_file_get_string(
        app->settings, "WindowManager", key, NULL);
    GError *error = NULL;
    g_key_file_set_string(app->settings, "WindowManager", key, value);
    if (!save_settings(app, &error)) {
        if (previous != NULL)
            g_key_file_set_string(app->settings, "WindowManager", key, previous);
        else
            g_key_file_remove_key(app->settings, "WindowManager", key, NULL);
        show_error(app, "Window preference was not saved", error->message);
        g_clear_error(&error);
    }
    g_free(previous);
}

static void
on_lock_timeout_changed(GtkComboBox *combo, gpointer user_data)
{
    SettingsApp *app = user_data;
    const gchar *selected = gtk_combo_box_get_active_id(combo);
    gchar *end = NULL;
    gint requested = selected != NULL ? (gint)g_ascii_strtoll(selected, &end, 10) : -1;
    gboolean had_value = g_key_file_has_key(
        app->settings, "LockScreen", "idle_minutes", NULL);
    gint previous = g_key_file_get_integer(
        app->settings, "LockScreen", "idle_minutes", NULL);
    GError *error = NULL;
    if (selected == NULL || end == selected || *end != '\0')
        return;
    if (app->updating_switches)
        return;
    g_key_file_set_integer(app->settings, "LockScreen", "idle_minutes",
                           requested);
    if (save_settings(app, &error))
        return;
    if (had_value)
        g_key_file_set_integer(app->settings, "LockScreen", "idle_minutes",
                               previous);
    else
        g_key_file_remove_key(app->settings, "LockScreen", "idle_minutes", NULL);
    show_error(app, "Lock screen preference was not saved",
               error ? error->message : "Settings could not be written.");
    g_clear_error(&error);
    gchar previous_text[16];
    g_snprintf(previous_text, sizeof(previous_text), "%d",
               had_value ? previous : 3);
    app->updating_switches = TRUE;
    gtk_combo_box_set_active_id(combo, previous_text);
    app->updating_switches = FALSE;
}

static gchar *
find_app_executable(SettingsApp *app, const gchar *app_dir,
                    const gchar *binary)
{
    const gchar *installed = NULL;
    if (g_str_equal(app_dir, "screenshoot-manager"))
        installed = "/usr/share/macos-de/apps/screenshoot-manager/screenshoot-manager";
    else if (g_str_equal(app_dir, "Notification-manager"))
        installed = "/usr/share/macos-de/apps/Notification-manager/Notification-manager";
    if (installed != NULL && g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE))
        return g_strdup(installed);

    gchar *executable_dir = g_path_get_dirname(app->executable_path);
    gchar *candidate = g_build_filename(
        executable_dir, "..", app_dir, binary, NULL);
    g_free(executable_dir);
    if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
        return candidate;
    g_free(candidate);

    candidate = g_find_program_in_path(binary);
    return candidate;
}

static void
launch_test_application(SettingsApp *app, const gchar *app_dir,
                        const gchar *binary, const gchar *argument)
{
    gchar *path = find_app_executable(app, app_dir, binary);
    if (path == NULL) {
        gchar *message = g_strdup_printf(
            "%s was not found. Build all desktop applications before testing.",
            binary);
        show_error(app, "Test application unavailable", message);
        g_free(message);
        return;
    }
    gchar *argv[] = { path, (gchar *)argument, NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        show_error(app, "Could not start test application", error->message);
        g_clear_error(&error);
    }
    g_free(path);
}

static void
test_notification(GtkButton *button, gpointer user_data)
{
    SettingsApp *app = user_data;
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    gboolean has_owner = FALSE;
    (void) button;
    if (bus == NULL) {
        show_error(app, "Could not connect to notifications",
                   error != NULL ? error->message :
                   "The session D-Bus connection is unavailable.");
        g_clear_error(&error);
        return;
    }

    GVariant *owner_reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.freedesktop.Notifications"),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (owner_reply == NULL) {
        show_error(app, "Could not check notification service",
                   error != NULL ? error->message :
                   "The session bus did not answer the service check.");
        g_clear_error(&error);
        g_object_unref(bus);
        return;
    }
    g_variant_get(owner_reply, "(b)", &has_owner);
    g_variant_unref(owner_reply);

    if (!has_owner) {
        gchar *service_path = find_app_executable(
            app, "Notification-manager", "Notification-manager");
        if (service_path == NULL) {
            show_error(app, "Notification service unavailable",
                       "Build or install the macOS-DE Notification Manager, then try again.");
            g_object_unref(bus);
            return;
        }
        gchar *service_argv[] = { service_path, "--daemon", NULL };
        if (!g_spawn_async(NULL, service_argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &error)) {
            show_error(app, "Could not start notification service",
                       error != NULL ? error->message :
                       "The Notification Manager could not be started.");
            g_clear_error(&error);
            g_free(service_path);
            g_object_unref(bus);
            return;
        }
        g_free(service_path);

        for (guint attempt = 0; attempt < 40 && !has_owner; attempt++) {
            g_usleep(50000);
            owner_reply = g_dbus_connection_call_sync(
                bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameHasOwner",
                g_variant_new("(s)", "org.freedesktop.Notifications"),
                G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL,
                &error);
            if (owner_reply == NULL)
                break;
            g_variant_get(owner_reply, "(b)", &has_owner);
            g_variant_unref(owner_reply);
        }
        if (!has_owner) {
            show_error(app, "Notification service did not start",
                       error != NULL ? error->message :
                       "The Notification Manager did not acquire its D-Bus service name.");
            g_clear_error(&error);
            g_object_unref(bus);
            return;
        }
    }

    GVariantBuilder actions;
    GVariantBuilder hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susss@as@a{sv}i)", "DE Settings", 0u,
                      "dialog-information", "Notification test",
                      "Your notification service is responding.",
                      g_variant_builder_end(&actions),
                      g_variant_builder_end(&hints), 5000),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2500, NULL, &error);
    if (reply == NULL) {
        show_error(app, "Notification test failed",
                   error ? error->message :
                   "No notification service replied to the test.");
        g_clear_error(&error);
    } else {
        g_variant_unref(reply);
    }
    g_object_unref(bus);
}

static void
lock_desktop_now(GtkButton *button, gpointer user_data)
{
    SettingsApp *app = user_data;
    GError *error = NULL;
    (void)button;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (bus == NULL) {
        show_error(app, "Could not connect to the desktop",
                   error ? error->message :
                   "The session D-Bus connection is unavailable.");
        g_clear_error(&error);
        return;
    }

    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.macosde.Desktop", "/org/macosde/Desktop",
        "org.macosde.Desktop", "Lock", NULL, G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    if (reply == NULL) {
        show_error(app, "Could not lock the desktop",
                   error ? error->message :
                   "The running desktop did not accept the lock request.");
        g_clear_error(&error);
    } else {
        g_variant_unref(reply);
    }
    g_object_unref(bus);
}

static void
on_screenshot_test_clicked(GtkButton *button, gpointer user_data)
{
    SettingsApp *app = user_data;
    launch_test_application(
        app, g_object_get_data(G_OBJECT(button), "test-app-dir"),
        g_object_get_data(G_OBJECT(button), "test-app-binary"),
        g_object_get_data(G_OBJECT(button), "test-app-argument"));
}

static gboolean
draw_test_cube(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    CubeAnimation *animation = user_data;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    double center_x = allocation.width / 2.0;
    double center_y = allocation.height / 2.0;
    double scale = MIN(allocation.width, allocation.height) * 0.22;
    double cosine = cos(animation->angle);
    double sine = sin(animation->angle);
    double tilt_cosine = cos(animation->angle * 0.7);
    double tilt_sine = sin(animation->angle * 0.7);
    double points[8][2];

    for (int i = 0; i < 8; i++) {
        double x = (i & 1) ? 1.0 : -1.0;
        double y = (i & 2) ? 1.0 : -1.0;
        double z = (i & 4) ? 1.0 : -1.0;
        double rotated_x = x * cosine - z * sine;
        double rotated_z = x * sine + z * cosine;
        double rotated_y = y * tilt_cosine - rotated_z * tilt_sine;
        rotated_z = y * tilt_sine + rotated_z * tilt_cosine;
        double perspective = 3.2 / (3.2 - rotated_z * 0.35);
        points[i][0] = center_x + rotated_x * scale * perspective;
        points[i][1] = center_y + rotated_y * scale * perspective;
    }

    static const int faces[6][4] = {
        {0, 1, 3, 2}, {4, 6, 7, 5}, {0, 2, 6, 4},
        {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}
    };
    static const double colors[6][3] = {
        {0.24, 0.58, 0.98}, {0.30, 0.74, 0.94},
        {0.38, 0.54, 0.91}, {0.43, 0.67, 0.99},
        {0.20, 0.44, 0.82}, {0.35, 0.62, 0.91}
    };
    for (int face = 0; face < 6; face++) {
        cairo_move_to(cr, points[faces[face][0]][0], points[faces[face][0]][1]);
        for (int corner = 1; corner < 4; corner++)
            cairo_line_to(cr, points[faces[face][corner]][0],
                          points[faces[face][corner]][1]);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, colors[face][0], colors[face][1],
                             colors[face][2]);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.04, 0.08, 0.16, 0.8);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    }
    return FALSE;
}

static gboolean
advance_test_cube(gpointer user_data)
{
    GtkWidget *area = GTK_WIDGET(user_data);
    if (!gtk_widget_get_realized(area))
        return G_SOURCE_REMOVE;
    CubeAnimation *animation = g_object_get_data(G_OBJECT(area), "cube-animation");
    if (animation == NULL)
        return G_SOURCE_REMOVE;
    animation->angle += 0.035;
    gtk_widget_queue_draw(area);
    return G_SOURCE_CONTINUE;
}

static void
stop_test_cube(GtkWidget *window, gpointer user_data)
{
    GtkWidget *area = GTK_WIDGET(user_data);
    guint source_id = GPOINTER_TO_UINT(
        g_object_get_data(G_OBJECT(area), "cube-timeout"));
    (void) window;
    if (source_id != 0)
        g_source_remove(source_id);
}

static void
show_window_manager_test(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *area = gtk_drawing_area_new();
    CubeAnimation *animation = g_new0(CubeAnimation, 1);
    (void) user_data;
    (void) button;

    gtk_window_set_title(GTK_WINDOW(window), "macOS-DE Window Manager Test");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 400);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(root), 16);
    gtk_container_add(GTK_CONTAINER(window), root);
    gtk_widget_set_size_request(area, 360, 280);
    g_object_set_data_full(G_OBJECT(area), "cube-animation", animation, g_free);
    g_signal_connect(area, "draw", G_CALLBACK(draw_test_cube), animation);
    GtkWidget *instructions = gtk_label_new(
        "Drag this window, resize its edges, and try its title-bar controls.");
    gtk_label_set_line_wrap(GTK_LABEL(instructions), TRUE);
    gtk_box_pack_start(GTK_BOX(root), area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), instructions, FALSE, FALSE, 0);
    guint source_id = g_timeout_add(16, advance_test_cube, area);
    g_object_set_data(G_OBJECT(area), "cube-timeout",
                      GUINT_TO_POINTER(source_id));
    g_signal_connect(window, "destroy", G_CALLBACK(stop_test_cube), area);
    gtk_widget_show_all(window);
}

static gchar *
find_default_wallpaper(SettingsApp *app)
{
    gchar *executable_dir = g_path_get_dirname(app->executable_path);
    gchar *candidate = g_build_filename(
        executable_dir, "..", "..", "assets", "wallpaper", "mac.png", NULL);
    g_free(executable_dir);
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
        return candidate;
    g_free(candidate);

    candidate = g_strdup("/usr/share/macos-de/assets/wallpaper/mac.png");
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
        return candidate;
    g_free(candidate);

    candidate = g_strdup("assets/wallpaper/mac.png");
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
        return candidate;
    g_free(candidate);
    return NULL;
}

static void
wallpaper_controls(SettingsApp *app, const gchar *key,
                   GtkWidget **image, GtkWidget **status)
{
    if (g_str_equal(key, "lock_wallpaper")) {
        *image = app->lock_wallpaper_image;
        *status = app->lock_wallpaper_status;
    } else {
        *image = app->wallpaper_image;
        *status = app->wallpaper_status;
    }
}

static void
set_wallpaper_preview(SettingsApp *app, const gchar *key,
                      const gchar *path, const gchar *status_text)
{
    GtkWidget *image;
    GtkWidget *status;
    wallpaper_controls(app, key, &image, &status);
    GError *error = NULL;
    GdkPixbuf *preview = gdk_pixbuf_new_from_file_at_scale(
        path, 720, 360, TRUE, &error);
    if (preview == NULL) {
        gchar *message = g_strdup_printf(
            "Could not load preview: %s",
            error ? error->message : "unknown image error");
        gtk_label_set_text(GTK_LABEL(status), message);
        g_free(message);
        g_clear_error(&error);
        return;
    }
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), preview);
    gtk_label_set_text(GTK_LABEL(status), status_text);
    g_object_unref(preview);
}

static gchar *
current_desktop_wallpaper(SettingsApp *app)
{
    gchar *wallpaper = g_key_file_get_string(
        app->settings, "Appearance", "wallpaper", NULL);
    if (wallpaper != NULL && g_file_test(wallpaper, G_FILE_TEST_IS_REGULAR))
        return wallpaper;
    g_free(wallpaper);
    return find_default_wallpaper(app);
}

static gchar *
get_widget_directory(void)
{
    return g_build_filename(g_get_user_data_dir(), "macos-de", "widgets", NULL);
}

static gboolean
valid_widget_id(const gchar *id)
{
    if (id == NULL || *id == '\0' || strlen(id) > 64)
        return FALSE;
    for (const gchar *cursor = id; *cursor != '\0'; cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '-' && *cursor != '_')
            return FALSE;
    }
    return TRUE;
}

static gchar *
find_widget_root(const gchar *directory)
{
    gchar *root_config = g_build_filename(directory, "config.cfg", NULL);
    if (g_file_test(root_config, G_FILE_TEST_IS_REGULAR)) {
        g_free(root_config);
        return g_strdup(directory);
    }
    g_free(root_config);
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (dir == NULL)
        return NULL;
    gchar *result = NULL;
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(directory, name, NULL);
        gchar *config = g_build_filename(child, "config.cfg", NULL);
        if (result == NULL && g_file_test(child, G_FILE_TEST_IS_DIR) &&
            g_file_test(config, G_FILE_TEST_IS_REGULAR))
            result = g_strdup(child);
        g_free(config);
        g_free(child);
        if (result != NULL)
            break;
    }
    g_dir_close(dir);
    return result;
}

static gboolean
validate_widget_package(const gchar *root, gchar **id_out,
                        gchar **name_out, GError **error)
{
    gchar *config_path = g_build_filename(root, "config.cfg", NULL);
    GKeyFile *config = g_key_file_new();
    gboolean valid = FALSE;
    gchar *id = NULL, *name = NULL, *executable = NULL;
    gchar *binary = NULL, *canonical_root = NULL, *canonical_binary = NULL;
    if (!g_key_file_load_from_file(config, config_path, G_KEY_FILE_NONE, error))
        goto done;
    id = g_key_file_get_string(config, "Widget", "id", error);
    if (id == NULL)
        goto done;
    name = g_key_file_get_string(config, "Widget", "name", error);
    if (name == NULL)
        goto done;
    executable = g_key_file_get_string(config, "Widget", "exec", error);
    if (executable == NULL)
        goto done;
    if (!valid_widget_id(id) || *name == '\0' ||
        g_path_is_absolute(executable) || strchr(executable, '\\') != NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Widget config.cfg has an invalid id, name, or executable path.");
        goto done;
    }
    canonical_root = g_canonicalize_filename(root, NULL);
    binary = g_build_filename(root, executable, NULL);
    canonical_binary = g_canonicalize_filename(binary, NULL);
    if (!g_str_has_prefix(canonical_binary, canonical_root) ||
        canonical_binary[strlen(canonical_root)] != G_DIR_SEPARATOR ||
        !g_file_test(canonical_binary, G_FILE_TEST_IS_REGULAR) ||
        !g_file_test(canonical_binary, G_FILE_TEST_IS_EXECUTABLE)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Widget executable must be an executable file inside the package.");
        goto done;
    }
    *id_out = g_strdup(id);
    *name_out = g_strdup(name);
    valid = TRUE;
done:
    g_free(config_path);
    g_key_file_unref(config);
    g_free(id);
    g_free(name);
    g_free(executable);
    g_free(binary);
    g_free(canonical_root);
    g_free(canonical_binary);
    return valid;
}

static gboolean
remove_widget_tree(GFile *file, GError **error)
{
    GFileInfo *info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        NULL, error);
    if (info == NULL)
        return FALSE;
    gboolean success = TRUE;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *children = g_file_enumerate_children(
            file, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            NULL, error);
        if (children == NULL) {
            g_object_unref(info);
            return FALSE;
        }
        GFileInfo *child_info;
        while (success && (child_info = g_file_enumerator_next_file(
                               children, NULL, error)) != NULL) {
            GFile *child = g_file_get_child(
                file, g_file_info_get_name(child_info));
            success = remove_widget_tree(child, error);
            g_object_unref(child);
            g_object_unref(child_info);
        }
        if (error != NULL && *error != NULL)
            success = FALSE;
        g_object_unref(children);
    }
    g_object_unref(info);
    return success && g_file_delete(file, NULL, error);
}

static void
on_widget_enabled_changed(GObject *object, GParamSpec *property,
                          gpointer user_data)
{
    SettingsApp *app = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(object), "widget-id");
    gchar *key = g_strdup_printf("enabled_%s", id);
    gboolean had_value = g_key_file_has_key(app->settings, "Widgets", key, NULL);
    gboolean previous = g_key_file_get_boolean(app->settings, "Widgets", key, NULL);
    gboolean requested = gtk_switch_get_active(GTK_SWITCH(object));
    GError *error = NULL;
    (void)property;
    if (app->updating_switches) {
        g_free(key);
        return;
    }
    g_key_file_set_boolean(app->settings, "Widgets", key, requested);
    if (save_settings(app, &error)) {
        g_free(key);
        return;
    }
    if (had_value)
        g_key_file_set_boolean(app->settings, "Widgets", key, previous);
    else
        g_key_file_remove_key(app->settings, "Widgets", key, NULL);
    app->updating_switches = TRUE;
    gtk_switch_set_active(GTK_SWITCH(object), previous);
    app->updating_switches = FALSE;
    show_error(app, "Widget setting was not saved",
               error ? error->message : "Settings could not be written.");
    g_clear_error(&error);
    g_free(key);
}

static void
on_remove_widget_clicked(GtkButton *button, gpointer user_data)
{
    SettingsApp *app = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "widget-id");
    gchar *directory = get_widget_directory();
    gchar *package = g_build_filename(directory, id, NULL);
    GFile *file = g_file_new_for_path(package);
    GError *error = NULL;
    if (!remove_widget_tree(file, &error)) {
        show_error(app, "Could not remove widget",
                   error ? error->message : "Widget package could not be removed.");
        g_clear_error(&error);
    } else {
        gchar *key = g_strdup_printf("enabled_%s", id);
        g_key_file_remove_key(app->settings, "Widgets", key, NULL);
        if (!save_settings(app, &error)) {
            show_error(app, "Widget removed, but settings could not be saved",
                       error ? error->message : "Settings could not be written.");
            g_clear_error(&error);
        }
        g_free(key);
        refresh_widget_status(app);
    }
    g_object_unref(file);
    g_free(package);
    g_free(directory);
}

static void
refresh_widget_status(SettingsApp *app)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->widget_status));
    for (GList *item = children; item != NULL; item = item->next)
        gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);

    gchar *directory = get_widget_directory();
    GDir *dir = g_dir_open(directory, 0, NULL);
    const gchar *entry;
    gboolean found = FALSE;
    if (dir != NULL) {
        while ((entry = g_dir_read_name(dir)) != NULL) {
            gchar *package = g_build_filename(directory, entry, NULL);
            if (!g_file_test(package, G_FILE_TEST_IS_DIR)) {
                g_free(package);
                continue;
            }
            gchar *root = find_widget_root(package);
            gchar *id = NULL, *name = NULL;
            GError *error = NULL;
            if (root != NULL && validate_widget_package(
                    root, &id, &name, &error)) {
                found = TRUE;
                GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                GtkWidget *label = gtk_label_new(name);
                gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
                gtk_widget_set_hexpand(label, TRUE);
                GtkWidget *toggle = gtk_switch_new();
                gchar *key = g_strdup_printf("enabled_%s", id);
                gtk_switch_set_active(GTK_SWITCH(toggle),
                    g_key_file_get_boolean(app->settings, "Widgets", key, NULL));
                g_object_set_data_full(G_OBJECT(toggle), "widget-id",
                                       g_strdup(id), g_free);
                g_signal_connect(toggle, "notify::active",
                                 G_CALLBACK(on_widget_enabled_changed), app);
                GtkWidget *remove = gtk_button_new_with_label("Remove");
                g_object_set_data_full(G_OBJECT(remove), "widget-id",
                                       g_strdup(id), g_free);
                g_signal_connect(remove, "clicked",
                                 G_CALLBACK(on_remove_widget_clicked), app);
                gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
                gtk_box_pack_start(GTK_BOX(row), toggle, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(app->widget_status), row,
                                   FALSE, FALSE, 4);
                gtk_widget_show_all(row);
                g_free(key);
            } else if (error != NULL) {
                g_warning("Ignoring invalid widget package %s: %s",
                          package, error->message);
            }
            g_clear_error(&error);
            g_free(id);
            g_free(name);
            g_free(root);
            g_free(package);
        }
        g_dir_close(dir);
    }
    if (!found) {
        GtkWidget *empty = gtk_label_new(
            "No installed widgets. Add a .widg ZIP package to get started.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0f);
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "muted");
        gtk_box_pack_start(GTK_BOX(app->widget_status), empty, FALSE, FALSE, 0);
        gtk_widget_show(empty);
    }
    g_free(directory);
}

static void
on_widget_bundle_response(GtkNativeDialog *native_dialog, gint response,
                          gpointer user_data)
{
    SettingsApp *app = user_data;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(native_dialog));
        gsize filename_length = filename != NULL ? strlen(filename) : 0;
        gboolean zip_file = filename_length >= 4 &&
            g_ascii_strcasecmp(filename + filename_length - 4, ".zip") == 0;
        gboolean widg_file = filename_length >= 5 &&
            g_ascii_strcasecmp(filename + filename_length - 5, ".widg") == 0;
        if (!zip_file && !widg_file) {
            show_error(app, "Unsupported widget bundle",
                       "Choose a ZIP widget archive with a .widg or .zip extension.");
        } else {
            GError *validation_error = NULL;
            if (!validate_widget_zip(filename, &validation_error)) {
                show_error(app, "Invalid widget bundle",
                           validation_error ? validation_error->message :
                           "The selected file is not a valid ZIP archive.");
                g_clear_error(&validation_error);
            } else {
                gchar *directory = get_widget_directory();
                GError *error = NULL;
                if (g_mkdir_with_parents(directory, 0700) != 0) {
                    g_set_error(&error, G_FILE_ERROR,
                                g_file_error_from_errno(errno),
                                "Could not create widget directory %s: %s",
                                directory, g_strerror(errno));
                }

                gchar *staging = g_dir_make_tmp("macos-de-widget-XXXXXX", &error);
                if (error == NULL) {
                    gchar *executable_dir = g_path_get_dirname(
                        app->executable_path);
                    gchar *helper = g_build_filename(
                        executable_dir, "..", "..", "bin",
                        "extract-widget.py", NULL);
                    gchar *argv[] = { (gchar *)"python3", helper, filename,
                                      staging, NULL };
                    gint exit_status = 0;
                    gchar *stderr_text = NULL;
                    gboolean spawned = g_spawn_sync(
                        NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                        NULL, &stderr_text, &exit_status, &error);
                    g_free(executable_dir);
                    g_free(helper);
                    if (spawned && exit_status != 0) {
                        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "%s", stderr_text && *stderr_text
                                    ? stderr_text : "Widget archive extraction failed.");
                    }
                    g_free(stderr_text);
                }
                gchar *root = error == NULL ? find_widget_root(staging) : NULL;
                gchar *id = NULL, *name = NULL;
                if (error == NULL && (root == NULL ||
                    !validate_widget_package(root, &id, &name, &error))) {
                    if (error == NULL)
                        g_set_error_literal(&error, G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "The archive must contain config.cfg and an executable package.");
                }
                if (error == NULL) {
                    gchar *destination_path = g_build_filename(directory, id, NULL);
                    gboolean replace = TRUE;
                    if (g_file_test(destination_path, G_FILE_TEST_EXISTS)) {
                        GtkWidget *confirm = gtk_message_dialog_new(
                            GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                            GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                            "Replace the existing widget?");
                        gtk_message_dialog_format_secondary_text(
                            GTK_MESSAGE_DIALOG(confirm), "%s", name);
                        replace = gtk_dialog_run(GTK_DIALOG(confirm)) ==
                                  GTK_RESPONSE_YES;
                        gtk_widget_destroy(confirm);
                    }
                    if (replace) {
                        if (g_file_test(destination_path, G_FILE_TEST_EXISTS)) {
                            GFile *existing = g_file_new_for_path(destination_path);
                            remove_widget_tree(existing, &error);
                            g_object_unref(existing);
                        }
                        if (error == NULL) {
                            GFile *source = g_file_new_for_path(root);
                            GFile *destination = g_file_new_for_path(destination_path);
                            if (g_file_move(source, destination, G_FILE_COPY_NONE,
                                            NULL, NULL, NULL, &error))
                                refresh_widget_status(app);
                            g_object_unref(source);
                            g_object_unref(destination);
                        }
                    }
                    g_free(destination_path);
                }
                if (error != NULL) {
                    show_error(app, "Could not add widget bundle", error->message);
                    g_clear_error(&error);
                }
                if (staging != NULL) {
                    GFile *temporary = g_file_new_for_path(staging);
                    GError *cleanup_error = NULL;
                    if (!remove_widget_tree(temporary, &cleanup_error)) {
                        g_warning("Could not remove temporary widget files: %s",
                                  cleanup_error ? cleanup_error->message :
                                  "unknown error");
                        g_clear_error(&cleanup_error);
                    }
                    g_object_unref(temporary);
                }
                g_free(root);
                g_free(id);
                g_free(name);
                g_free(staging);
                g_free(directory);
            }
        }
        g_free(filename);
    }
    g_object_unref(native_dialog);
}

static void
choose_widget_bundle(GtkButton *button, gpointer user_data)
{
    SettingsApp *app = user_data;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Add a widget bundle", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN, "Add", "Cancel");
    GtkFileFilter *filter = gtk_file_filter_new();
    (void)button;
    gtk_file_filter_set_name(filter, "Widget packages");
    gtk_file_filter_add_mime_type(filter, "application/zip");
    gtk_file_filter_add_pattern(filter, "*.zip");
    gtk_file_filter_add_pattern(filter, "*.ZIP");
    gtk_file_filter_add_pattern(filter, "*.widg");
    gtk_file_filter_add_pattern(filter, "*.WIDG");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    g_signal_connect(chooser, "response",
                     G_CALLBACK(on_widget_bundle_response), app);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

static gboolean
validate_widget_zip(const gchar *path, GError **error)
{
    GFile *file = g_file_new_for_path(path);
    GFileInputStream *stream = g_file_read(file, NULL, error);
    g_object_unref(file);
    if (stream == NULL)
        return FALSE;

    guchar header[4];
    gsize bytes_read = 0;
    gboolean read_ok = g_input_stream_read_all(
        G_INPUT_STREAM(stream), header, sizeof(header), &bytes_read,
        NULL, error);
    g_input_stream_close(G_INPUT_STREAM(stream), NULL, NULL);
    g_object_unref(stream);
    if (!read_ok)
        return FALSE;
    if (bytes_read == sizeof(header) && header[0] == 'P' && header[1] == 'K' &&
        ((header[2] == 3 && header[3] == 4) ||
         (header[2] == 5 && header[3] == 6) ||
         (header[2] == 7 && header[3] == 8)))
        return TRUE;

    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "The selected file does not have a valid ZIP header.");
    return FALSE;
}

static void
on_wallpaper_response(GtkNativeDialog *native_dialog, gint response, gpointer user_data)
{
    SettingsApp *app = user_data;
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(native_dialog);
    const gchar *key = g_object_get_data(
        G_OBJECT(native_dialog), "wallpaper-key");
    if (key == NULL)
        key = "wallpaper";
    GtkWidget *image;
    GtkWidget *status;
    wallpaper_controls(app, key, &image, &status);

    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(chooser);
        GError *error = NULL;
        GdkPixbuf *preview = gdk_pixbuf_new_from_file_at_scale(filename, 720, 360, TRUE, &error);

        if (preview == NULL) {
            show_error(app, "Could not use this wallpaper",
                       error ? error->message : "The image could not be loaded.");
            g_clear_error(&error);
        } else {
            gchar *absolute_path = g_canonicalize_filename(filename, NULL);
            gchar *previous_path = g_key_file_get_string(
                app->settings, "Appearance", key, NULL);
            g_key_file_set_string(app->settings, "Appearance", key,
                                  absolute_path);
            if (save_settings(app, &error)) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(image), preview);
                gtk_label_set_text(GTK_LABEL(status), absolute_path);
            } else {
                if (previous_path != NULL)
                    g_key_file_set_string(app->settings, "Appearance", key,
                                          previous_path);
                else
                    g_key_file_remove_key(app->settings, "Appearance", key, NULL);
                show_error(app, "Wallpaper was not saved",
                           error ? error->message :
                           "Settings could not be written.");
                g_clear_error(&error);
            }
            g_free(absolute_path);
            g_free(previous_path);
            g_object_unref(preview);
        }
        g_free(filename);
    }

    g_object_unref(native_dialog);
}

static void
choose_wallpaper(GtkButton *button, gpointer user_data)
{
    SettingsApp *app = user_data;
    const gchar *key = g_object_get_data(G_OBJECT(button), "wallpaper-key");
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Choose a wallpaper", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "Choose", "Cancel");
    GtkFileFilter *filter = gtk_file_filter_new();
    (void) button;

    gtk_file_filter_set_name(filter, "Image files");
    gtk_file_filter_add_mime_type(filter, "image/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    g_object_set_data_full(G_OBJECT(chooser), "wallpaper-key",
                           g_strdup(key != NULL ? key : "wallpaper"), g_free);
    g_signal_connect(chooser, "response", G_CALLBACK(on_wallpaper_response), app);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

static void
clear_wallpaper(GtkButton *button, gpointer user_data)
{
    SettingsApp *app = user_data;
    const gchar *key = g_object_get_data(G_OBJECT(button), "wallpaper-key");
    if (key == NULL)
        key = "wallpaper";
    GError *error = NULL;
    gchar *default_wallpaper = g_str_equal(key, "lock_wallpaper")
        ? current_desktop_wallpaper(app) : find_default_wallpaper(app);
    gchar *previous_path = g_key_file_get_string(
        app->settings, "Appearance", key, NULL);

    if (default_wallpaper == NULL) {
        show_error(app, "Default wallpaper unavailable",
                   "The fallback wallpaper image could not be found.");
        g_free(previous_path);
        return;
    }
    g_key_file_remove_key(app->settings, "Appearance", key, NULL);
    if (!save_settings(app, &error)) {
        if (previous_path != NULL)
            g_key_file_set_string(app->settings, "Appearance", key,
                                  previous_path);
        show_error(app, "Wallpaper preference was not cleared",
                   error ? error->message :
                   "Settings could not be written.");
        g_clear_error(&error);
        g_free(previous_path);
        g_free(default_wallpaper);
        return;
    }
    g_free(previous_path);
    set_wallpaper_preview(app, key, default_wallpaper,
        g_str_equal(key, "lock_wallpaper")
            ? "Using the desktop wallpaper for the lock screen."
            : "Using the default wallpaper (mac.png).");
    g_free(default_wallpaper);
}

static void
open_uri(GtkButton *button, gpointer user_data)
{
    const gchar *uri = g_object_get_data(G_OBJECT(button), "settings-uri");
    GError *error = NULL;
    (void) user_data;

    if (!gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not open this location");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
    }
}

static gboolean
launch_first_available(const gchar *const *programs, GError **error)
{
    for (guint i = 0; programs[i] != NULL; i++) {
        gchar *path = g_find_program_in_path(programs[i]);
        if (path != NULL) {
            gchar *argv[] = { path, NULL };
            gboolean launched = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                              NULL, NULL, NULL, error);
            g_free(path);
            return launched;
        }
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "No compatible application is installed.");
    return FALSE;
}

static void
launch_system_monitor(GtkButton *button, gpointer user_data)
{
    static const gchar *const programs[] = {
        "gnome-system-monitor", "mate-system-monitor", "xfce4-taskmanager", "ksysguard", NULL
    };
    GError *error = NULL;
    (void) button;
    (void) user_data;

    if (!launch_first_available(programs, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
            "System monitor is unavailable");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
    }
}

static void
launch_terminal(GtkButton *button, gpointer user_data)
{
    static const gchar *const programs[] = {
        "gnome-terminal", "xfce4-terminal", "konsole", "mate-terminal", "xterm", NULL
    };
    GError *error = NULL;
    (void) button;
    (void) user_data;

    if (!launch_first_available(programs, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
            "Terminal is unavailable");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
    }
}

static void
add_info_row(GtkWidget *grid, guint row, const gchar *label, const gchar *value)
{
    GtkWidget *name = gtk_label_new(label);
    GtkWidget *detail = gtk_label_new(value);
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_widget_set_halign(detail, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(name), "muted");
    gtk_style_context_add_class(gtk_widget_get_style_context(detail), "setting-title");
    gtk_grid_attach(GTK_GRID(grid), name, 0, (gint) row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), detail, 1, (gint) row, 1, 1);
}

static gchar *
read_os_name(void)
{
    gchar *contents = NULL;
    gchar **lines;
    gchar *name = NULL;

    if (!g_file_get_contents("/etc/os-release", &contents, NULL, NULL))
        return g_strdup("Linux");
    lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix(lines[i], "PRETTY_NAME=")) {
            name = g_strdup(lines[i] + strlen("PRETTY_NAME="));
            if (name[0] == '"' || name[0] == '\'') {
                gsize len = strlen(name);
                if (len > 1)
                    name[len - 1] = '\0';
                memmove(name, name + 1, len);
            }
            break;
        }
    }
    g_strfreev(lines);
    g_free(contents);
    return name != NULL ? name : g_strdup("Linux");
}

static gchar *
read_memory_summary(void)
{
    gchar *contents = NULL;
    gchar **lines;
    gchar *result = NULL;

    if (!g_file_get_contents("/proc/meminfo", &contents, NULL, NULL))
        return g_strdup("Unavailable");
    lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        guint64 kilobytes;
        if (sscanf(lines[i], "MemTotal: %" G_GUINT64_FORMAT " kB", &kilobytes) == 1) {
            result = g_strdup_printf("%.1f GB", (gdouble) kilobytes / 1048576.0);
            break;
        }
    }
    g_strfreev(lines);
    g_free(contents);
    return result != NULL ? result : g_strdup("Unavailable");
}

static GtkWidget *
make_header(const gchar *logo_path)
{
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title = gtk_label_new("Settings");
    GtkWidget *subtitle = gtk_label_new("Make this desktop yours");

    if (logo_path != NULL) {
        GError *error = NULL;
        GdkPixbuf *logo = gdk_pixbuf_new_from_file_at_scale(logo_path, 42, 42, TRUE, &error);
        if (logo != NULL) {
            GtkWidget *image = gtk_image_new_from_pixbuf(logo);
            gtk_box_pack_start(GTK_BOX(header), image, FALSE, FALSE, 0);
            g_object_unref(logo);
        } else {
            g_clear_error(&error);
        }
    }

    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "app-title");
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
    gtk_box_pack_start(GTK_BOX(titles), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(titles), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), titles, TRUE, TRUE, 0);
    gtk_widget_set_margin_start(header, 24);
    gtk_widget_set_margin_end(header, 24);
    gtk_widget_set_margin_top(header, 18);
    gtk_widget_set_margin_bottom(header, 14);
    return header;
}

static void
load_saved_preferences(SettingsApp *app)
{
    GError *error = NULL;
    gboolean valid_theme;

    app->settings = g_key_file_new();
    if (!g_key_file_load_from_file(app->settings, app->settings_path, G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
            !g_error_matches(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_UNKNOWN_ENCODING) &&
            !g_error_matches(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE))
            g_warning("Could not load settings: %s", error->message);
        g_clear_error(&error);
    }

    gchar *theme = g_key_file_get_string(app->settings, "Appearance", "theme", NULL);
    valid_theme = g_strcmp0(theme, "dark") == 0 || g_strcmp0(theme, "light") == 0;
    app->dark_theme = theme == NULL || g_strcmp0(theme, "light") != 0;
    g_free(theme);

    if (!valid_theme) {
        GError *save_error = NULL;
        g_key_file_set_string(app->settings, "Appearance", "theme",
                              app->dark_theme ? "dark" : "light");
        if (!save_settings(app, &save_error)) {
            g_warning("Could not save the default theme setting: %s", save_error->message);
            g_clear_error(&save_error);
        }
    }

    const gchar *wallpaper_keys[] = { "wallpaper", "lock_wallpaper" };
    for (guint i = 0; i < G_N_ELEMENTS(wallpaper_keys); i++) {
        gchar *wallpaper = g_key_file_get_string(
            app->settings, "Appearance", wallpaper_keys[i], NULL);
        if (wallpaper != NULL && wallpaper[0] != '\0' &&
            !g_path_is_absolute(wallpaper)) {
            g_warning("Ignoring non-absolute %s path in settings: %s",
                      wallpaper_keys[i], wallpaper);
            g_key_file_remove_key(
                app->settings, "Appearance", wallpaper_keys[i], NULL);
        }
        g_free(wallpaper);
    }
}

static GtkWidget *
create_wallpaper_card(SettingsApp *app, const gchar *key,
                      const gchar *title, const gchar *description)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *expander = gtk_expander_new(NULL);
    GtkWidget *heading = gtk_label_new(title);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *detail = gtk_label_new(description);
    gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                "settings-card");
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),
                                "card-title");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_expander_set_label_widget(GTK_EXPANDER(expander), heading);
    gtk_expander_set_expanded(GTK_EXPANDER(expander), FALSE);
    gtk_label_set_line_wrap(GTK_LABEL(detail), TRUE);
    gtk_widget_set_halign(detail, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(detail), "muted");
    gtk_box_pack_start(GTK_BOX(content), detail, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(expander), content);
    gtk_box_pack_start(GTK_BOX(card), expander, FALSE, FALSE, 0);
    GtkWidget **image_slot = g_str_equal(key, "lock_wallpaper")
        ? &app->lock_wallpaper_image : &app->wallpaper_image;
    GtkWidget **status_slot = g_str_equal(key, "lock_wallpaper")
        ? &app->lock_wallpaper_status : &app->wallpaper_status;
    *image_slot = gtk_image_new();
    gtk_widget_set_size_request(*image_slot, -1, 200);
    gtk_widget_set_halign(*image_slot, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(content), *image_slot, FALSE, FALSE, 2);
    *status_slot = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(*status_slot), TRUE);
    gtk_label_set_xalign(GTK_LABEL(*status_slot), 0.0f);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(*status_slot), "muted");
    gtk_box_pack_start(GTK_BOX(content), *status_slot, FALSE, FALSE, 0);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *button = gtk_button_new_with_label("Choose Image…");
    g_object_set_data(G_OBJECT(button), "wallpaper-key", (gpointer)key);
    g_signal_connect(button, "clicked", G_CALLBACK(choose_wallpaper), app);
    gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
    button = gtk_button_new_with_label("Clear Selection");
    g_object_set_data(G_OBJECT(button), "wallpaper-key", (gpointer)key);
    g_signal_connect(button, "clicked", G_CALLBACK(clear_wallpaper), app);
    gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), actions, FALSE, FALSE, 0);
    return card;
}

static void
load_wallpaper_card_preview(SettingsApp *app, const gchar *key)
{
    gchar *selected = g_key_file_get_string(
        app->settings, "Appearance", key, NULL);
    if (selected != NULL && *selected != '\0') {
        set_wallpaper_preview(app, key, selected, selected);
        g_free(selected);
        return;
    }
    g_free(selected);

    gchar *fallback = g_str_equal(key, "lock_wallpaper")
        ? current_desktop_wallpaper(app) : find_default_wallpaper(app);
    if (fallback == NULL) {
        GtkWidget *image;
        GtkWidget *status;
        wallpaper_controls(app, key, &image, &status);
        gtk_label_set_text(GTK_LABEL(status), "Wallpaper image is unavailable.");
        return;
    }
    set_wallpaper_preview(app, key, fallback,
        g_str_equal(key, "lock_wallpaper")
            ? "Using the desktop wallpaper for the lock screen."
            : "Using the default wallpaper (mac.png).");
    g_free(fallback);
}

static void
activate(GtkApplication *application, gpointer user_data)
{
    SettingsApp *app = user_data;
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *notebook = gtk_notebook_new();
    GtkWidget *page;
    GtkWidget *card;
    GtkWidget *toggle;
    GtkWidget *grid;
    GtkWidget *button;
    gchar *logo_path = find_asset("logo.png", app->executable_path);
    gchar *css_path;
    (void) application;

    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "DE Settings");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 940, 720);
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->window), "settings-window");
    css_path = find_asset("DE-settings.css", app->executable_path);
    if (css_path != NULL) {
        GtkCssProvider *provider = gtk_css_provider_new();
        GError *css_error = NULL;
        if (gtk_css_provider_load_from_path(provider, css_path, &css_error)) {
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        } else {
            g_warning("Could not load settings styles: %s", css_error->message);
            g_clear_error(&css_error);
        }
        g_object_unref(provider);
        g_free(css_path);
    }
    gtk_box_pack_start(GTK_BOX(outer), make_header(logo_path), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), notebook, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), outer);

    page = make_page(14);
    card = make_card("Appearance", "Choose the look of your desktop. The theme preference is saved for desktop-wide integration.");
    toggle = gtk_switch_new();
    app->theme_switch = toggle;
    gtk_switch_set_active(GTK_SWITCH(toggle), app->dark_theme);
    add_setting_row(card, "Dark appearance", "Dark is the default. Turn this off to use the light appearance.", toggle);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    g_signal_connect(toggle, "notify::active", G_CALLBACK(on_theme_changed), app);

    card = make_card("Window manager appearance",
                     "Choose the window controls and corner style used by the desktop.");
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "circles", "Circles");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "boxes", "Boxes");
    gchar *button_shape = g_key_file_get_string(
        app->settings, "WindowManager", "buttons", NULL);
    if (!button_shape ||
        (!g_str_equal(button_shape, "circles") &&
         !g_str_equal(button_shape, "boxes"))) {
        g_free(button_shape);
        button_shape = g_strdup("circles");
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), button_shape);
    g_free(button_shape);
    g_object_set_data(G_OBJECT(combo), "settings-key", "buttons");
    app->button_shape = combo;
    add_setting_row(card, "Window control buttons", "Use circular or square close, minimize, and maximize buttons.", combo);
    g_signal_connect(combo, "changed",
                     G_CALLBACK(on_window_manager_preference_changed), app);

    combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "rounded", "Rounded");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "sharp", "Sharp");
    gchar *corner_style = g_key_file_get_string(
        app->settings, "WindowManager", "corners", NULL);
    if (!corner_style ||
        (!g_str_equal(corner_style, "rounded") &&
         !g_str_equal(corner_style, "sharp"))) {
        g_free(corner_style);
        corner_style = g_strdup("rounded");
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), corner_style);
    g_free(corner_style);
    g_object_set_data(G_OBJECT(combo), "settings-key", "corners");
    app->window_corners = combo;
    add_setting_row(card, "Window corners", "Choose rounded or sharp window corners.", combo);
    g_signal_connect(combo, "changed",
                     G_CALLBACK(on_window_manager_preference_changed), app);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

    card = make_card("Lock screen",
                     "Lock macOS-DE after inactivity. The display blanks after three more minutes; use your Linux account password to unlock.");
    combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "3", "3 minutes");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "15", "15 minutes");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "30", "30 minutes");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "60", "1 hour");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "120", "2 hours");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "0", "Never");
    gint lock_timeout = g_key_file_get_integer(
        app->settings, "LockScreen", "idle_minutes", NULL);
    gboolean automatic_lock_enabled =
        !current_user_disables_automatic_lock();
    if (!automatic_lock_enabled)
        lock_timeout = 0;
    else if (lock_timeout != 0 && lock_timeout != 3 && lock_timeout != 15 &&
        lock_timeout != 30 && lock_timeout != 60 && lock_timeout != 120)
        lock_timeout = 3;
    gchar lock_timeout_text[16];
    g_snprintf(lock_timeout_text, sizeof(lock_timeout_text), "%d",
               lock_timeout == 0 && !g_key_file_has_key(
                   app->settings, "LockScreen", "idle_minutes", NULL)
                   ? 3 : lock_timeout);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), lock_timeout_text);
    gtk_widget_set_sensitive(combo, automatic_lock_enabled);
    app->lock_timeout = combo;
    add_setting_row(card, "Lock after inactivity",
                    automatic_lock_enabled
                        ? "Require your Linux account password to return to the desktop."
                        : "Automatic locking is disabled for this system session.",
                    combo);
    g_signal_connect(combo, "changed",
                     G_CALLBACK(on_lock_timeout_changed), app);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new("Preferences"));

    page = make_page(14);
    card = create_wallpaper_card(
        app, "wallpaper", "Desktop wallpaper",
        "Choose the image shown behind your desktop and windows.");
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    card = create_wallpaper_card(
        app, "lock_wallpaper", "Lock screen",
        "Choose a separate image for the blurred lock-screen background. Clear Selection follows the desktop wallpaper.");
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    load_wallpaper_card_preview(app, "wallpaper");
    load_wallpaper_card_preview(app, "lock_wallpaper");
    card = make_card("Widgets", "Enable built-in System Stats or install native executable widgets from .widg ZIP packages. Widget programs run as your user and are not sandboxed.");
    toggle = gtk_switch_new();
    app->stats_switch = toggle;
    gtk_switch_set_active(
        GTK_SWITCH(toggle),
        g_key_file_get_boolean(app->settings, "Widgets", "stats_enabled", NULL));
    add_setting_row(card, "System Stats",
                    "Show a movable desktop widget with CPU, RAM, and available GPU usage.",
                    toggle);
    g_signal_connect(toggle, "notify::active",
                     G_CALLBACK(on_stats_widget_changed), app);
    app->widget_status = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    refresh_widget_status(app);
    gtk_box_pack_start(GTK_BOX(card), app->widget_status, FALSE, FALSE, 0);
    button = gtk_button_new_with_label("Add Widget…");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_signal_connect(button, "clicked", G_CALLBACK(choose_widget_bundle), app);
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new("Wallpaper And Widgets"));

    page = make_page(14);
    card = make_card("Hardware overview", "A quick, read-only summary of this computer.");
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 36);
    struct utsname system_info;
    if (uname(&system_info) == 0) {
        gchar *os_name = read_os_name();
        add_info_row(grid, 0, "Operating system", os_name);
        g_free(os_name);
        add_info_row(grid, 1, "Kernel", system_info.release);
        add_info_row(grid, 2, "Architecture", system_info.machine);
    } else {
        add_info_row(grid, 0, "Operating system", "Unavailable");
    }
    gchar *processors = g_strdup_printf("%u logical processors", g_get_num_processors());
    add_info_row(grid, 3, "CPU", processors);
    g_free(processors);
    gchar *memory = read_memory_summary();
    add_info_row(grid, 4, "Installed memory", memory);
    g_free(memory);
    gchar *display = g_strdup_printf("%d display%s", gdk_display_get_n_monitors(gdk_display_get_default()),
                                     gdk_display_get_n_monitors(gdk_display_get_default()) == 1 ? "" : "s");
    add_info_row(grid, 5, "Displays", display);
    g_free(display);
    gtk_box_pack_start(GTK_BOX(card), grid, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    card = make_card("Hardware tools", "Open a system monitor to inspect processes and live resource use.");
    button = gtk_button_new_with_label("Open System Monitor");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_signal_connect(button, "clicked", G_CALLBACK(launch_system_monitor), app);
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new("Hardware"));

    page = make_page(14);
    card = make_card("Administration", "Quick access to desktop configuration and system administration tools.");
    button = gtk_button_new_with_label("Open macOS-DE configuration folder");
    gchar *config_dir = g_path_get_dirname(app->settings_path);
    gchar *config_uri = g_filename_to_uri(config_dir, NULL, NULL);
    g_object_set_data_full(G_OBJECT(button), "settings-uri", config_uri, g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(open_uri), app);
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    button = gtk_button_new_with_label("Open Terminal");
    g_signal_connect(button, "clicked", G_CALLBACK(launch_terminal), app);
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    g_free(config_dir);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

    card = make_card("Settings file", "Preferences are stored locally in a standard INI file. Changes are written immediately.");
    GtkWidget *path_label = gtk_label_new(app->settings_path);
    gtk_label_set_xalign(GTK_LABEL(path_label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(path_label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(path_label), "muted");
    gtk_box_pack_start(GTK_BOX(card), path_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new("Administration"));

    page = make_page(14);
    card = make_card("Window manager",
                     "Open a test window with a rotating 3D cube to try moving, resizing, and window controls.");
    button = gtk_button_new_with_label("Open spinning-cube test window");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_signal_connect(button, "clicked", G_CALLBACK(show_window_manager_test), app);
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

    card = make_card("Lock screen",
                     "Immediately open the lock screen to test its appearance and password prompt.");
    button = gtk_button_new_with_label("Lock screen now");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_signal_connect(button, "clicked", G_CALLBACK(lock_desktop_now), app);
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

    card = make_card("Notifications",
                     "Send a sample notification. The macOS-DE notification service starts automatically if no service is running.");
    button = gtk_button_new_with_label("Send test notification");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_signal_connect(button, "clicked", G_CALLBACK(test_notification), app);
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

    card = make_card("Screenshots",
                     "Capture the full desktop, save it in Pictures, and copy the PNG to the clipboard.");
    button = gtk_button_new_with_label("Take test screenshot");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_signal_connect(button, "clicked",
                     G_CALLBACK(on_screenshot_test_clicked), app);
    g_object_set_data(G_OBJECT(button), "test-app-dir", "screenshoot-manager");
    g_object_set_data(G_OBJECT(button), "test-app-binary", "screenshoot-manager");
    g_object_set_data(G_OBJECT(button), "test-app-argument", "--capture");
    gtk_box_pack_start(GTK_BOX(card), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new("Dev"));

    if (requested_settings_page != NULL) {
        gint page_count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
        for (gint i = 0; i < page_count; i++) {
            GtkWidget *tab = gtk_notebook_get_tab_label(
                GTK_NOTEBOOK(notebook),
                gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i));
            const gchar *label = GTK_IS_LABEL(tab)
                ? gtk_label_get_text(GTK_LABEL(tab)) : "";
            if ((g_str_equal(requested_settings_page, "wallpaper") ||
                 g_str_equal(requested_settings_page, "widgets")) &&
                g_str_equal(label, "Wallpaper And Widgets")) {
                gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), i);
                break;
            }
        }
    }

    apply_theme(app);
    gtk_widget_show_all(app->window);
    g_free(logo_path);
}

int
main(int argc, char **argv)
{
    SettingsApp app = {0};
    GtkApplication *application;
    gint status;
    GOptionEntry options[] = {
        { "page", 'p', 0, G_OPTION_ARG_STRING, &requested_settings_page,
          "Open a Settings page", "PAGE" },
        { NULL }
    };

    app.settings_path = get_settings_path();
    app.executable_path = g_file_read_link("/proc/self/exe", NULL);
    if (app.executable_path == NULL) {
        app.executable_path = g_find_program_in_path(argv[0]);
        if (app.executable_path == NULL)
            app.executable_path = g_canonicalize_filename(argv[0], NULL);
    }
    load_saved_preferences(&app);
    application = gtk_application_new("org.macos-de.Settings", G_APPLICATION_DEFAULT_FLAGS);
    g_application_add_main_option_entries(G_APPLICATION(application), options);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    g_key_file_unref(app.settings);
    g_free(app.settings_path);
    g_free(app.executable_path);
    g_free(requested_settings_page);
    return status;
}
