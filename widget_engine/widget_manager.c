#include "widget_manager.h"
#include "../widgets/stats/stats.h"

typedef struct {
    gatomicrefcount refs;
    GSubprocess *child;
    gchar *binary;
    gboolean exited;
} WidgetProcess;

typedef struct {
    GtkWidget *stats;
    gchar *settings_path;
    gchar *widgets_path;
    GHashTable *external_widgets;
    gboolean closing;
    gboolean enabled;
    gboolean dragging;
    gint drag_x;
    gint drag_y;
    gint widget_x;
    gint widget_y;
    guint sync_source;
} WidgetManager;

static WidgetProcess *
widget_process_ref(WidgetProcess *process)
{
    g_atomic_ref_count_inc(&process->refs);
    return process;
}

static void
widget_process_unref(gpointer data)
{
    WidgetProcess *process = data;
    if (g_atomic_ref_count_dec(&process->refs)) {
        g_object_unref(process->child);
        g_free(process->binary);
        g_free(process);
    }
}

static void
on_widget_process_finished(GObject *object, GAsyncResult *result,
                           gpointer user_data)
{
    WidgetProcess *process = user_data;
    GError *error = NULL;
    if (!g_subprocess_wait_finish(G_SUBPROCESS(object), result, &error)) {
        g_warning("Could not wait for widget process: %s", error->message);
        g_clear_error(&error);
    }
    process->exited = TRUE;
    widget_process_unref(process);
}

static gboolean
load_external_widget(const gchar *package_path, const gchar *id,
                     gchar **binary_out)
{
    gchar *config_path = g_build_filename(package_path, "config.cfg", NULL);
    GKeyFile *config = g_key_file_new();
    GError *error = NULL;
    gchar *configured_id = NULL, *executable = NULL, *binary = NULL;
    gboolean valid = FALSE;
    if (!g_key_file_load_from_file(config, config_path, G_KEY_FILE_NONE, &error))
        goto done;
    configured_id = g_key_file_get_string(config, "Widget", "id", &error);
    if (configured_id == NULL || g_strcmp0(configured_id, id) != 0)
        goto done;
    executable = g_key_file_get_string(config, "Widget", "exec", &error);
    if (executable == NULL || *executable == '\0' ||
        g_path_is_absolute(executable) || strchr(executable, '\\') != NULL)
        goto done;
    gchar *canonical_package = g_canonicalize_filename(package_path, NULL);
    binary = g_build_filename(package_path, executable, NULL);
    gchar *canonical_binary = g_canonicalize_filename(binary, NULL);
    valid = g_str_has_prefix(canonical_binary, canonical_package) &&
        canonical_binary[strlen(canonical_package)] == G_DIR_SEPARATOR &&
        g_file_test(canonical_binary, G_FILE_TEST_IS_REGULAR) &&
        g_file_test(canonical_binary, G_FILE_TEST_IS_EXECUTABLE);
    if (valid)
        *binary_out = g_strdup(canonical_binary);
    g_free(canonical_package);
    g_free(canonical_binary);
done:
    g_clear_error(&error);
    g_free(config_path);
    g_free(configured_id);
    g_free(executable);
    g_free(binary);
    g_key_file_unref(config);
    return valid;
}

static gboolean
widget_id_is_valid(const gchar *id)
{
    if (id == NULL || *id == '\0' || strlen(id) > 64)
        return FALSE;
    for (const gchar *cursor = id; *cursor != '\0'; cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '-' && *cursor != '_')
            return FALSE;
    }
    return TRUE;
}

static void
stop_external_widget(WidgetManager *manager, const gchar *id)
{
    WidgetProcess *process = g_hash_table_lookup(manager->external_widgets, id);
    if (process != NULL) {
        g_subprocess_force_exit(process->child);
        g_hash_table_remove(manager->external_widgets, id);
    }
}

static void
sync_external_widgets(WidgetManager *manager)
{
    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(settings, manager->settings_path,
                                   G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        g_key_file_unref(settings);
        return;
    }

    GDir *directory = g_dir_open(manager->widgets_path, 0, NULL);
    if (directory != NULL) {
        const gchar *id;
        while ((id = g_dir_read_name(directory)) != NULL) {
            if (!widget_id_is_valid(id))
                continue;
            gchar *enabled_key = g_strdup_printf("enabled_%s", id);
            gboolean enabled = g_key_file_get_boolean(
                settings, "Widgets", enabled_key, NULL);
            g_free(enabled_key);
            gchar *package = g_build_filename(manager->widgets_path, id, NULL);
            gchar *binary = NULL;
            gboolean valid = g_file_test(package, G_FILE_TEST_IS_DIR) &&
                load_external_widget(package, id, &binary);
            WidgetProcess *running = g_hash_table_lookup(
                manager->external_widgets, id);
            if (!enabled || !valid ||
                (running != NULL && valid &&
                 g_strcmp0(running->binary, binary) != 0)) {
                if (running != NULL)
                    stop_external_widget(manager, id);
                running = NULL;
            }
            if (enabled && valid && running == NULL) {
                GSubprocessLauncher *launcher = g_subprocess_launcher_new(
                    G_SUBPROCESS_FLAGS_NONE);
                g_subprocess_launcher_set_cwd(launcher, package);
                const gchar *argv[] = { binary, NULL };
                GSubprocess *child = g_subprocess_launcher_spawnv(
                    launcher, argv, &error);
                g_object_unref(launcher);
                if (child != NULL) {
                    WidgetProcess *process = g_new0(WidgetProcess, 1);
                    g_atomic_ref_count_init(&process->refs);
                    process->child = child;
                    process->binary = g_strdup(binary);
                    g_hash_table_insert(manager->external_widgets,
                                        g_strdup(id), process);
                    g_subprocess_wait_async(child, NULL,
                        on_widget_process_finished,
                        widget_process_ref(process));
                } else {
                    g_warning("Could not launch widget %s: %s", id,
                              error->message);
                    g_clear_error(&error);
                }
            }
            g_free(binary);
            g_free(package);
        }
        g_dir_close(directory);
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, manager->external_widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gchar *enabled_key = g_strdup_printf("enabled_%s", (gchar *)key);
        gboolean enabled = g_key_file_get_boolean(
            settings, "Widgets", enabled_key, NULL);
        g_free(enabled_key);
        gchar *package = g_build_filename(manager->widgets_path,
                                          (gchar *)key, NULL);
        if (!enabled || !g_file_test(package, G_FILE_TEST_IS_DIR)) {
            WidgetProcess *process = value;
            g_subprocess_force_exit(process->child);
            g_hash_table_iter_remove(&iter);
        }
        g_free(package);
    }
    g_key_file_unref(settings);
}

static void
set_stats_visible(WidgetManager *manager, gboolean visible)
{
    if (visible) {
        gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(manager->stats)));
        gtk_widget_show(manager->stats);
    } else {
        gtk_widget_hide(manager->stats);
    }
}

static void
move_stats_widget(WidgetManager *manager, gint x, gint y)
{
    manager->widget_x = MAX(0, x);
    manager->widget_y = MAX(30, y);
    gtk_widget_set_margin_start(manager->stats, manager->widget_x);
    gtk_widget_set_margin_top(manager->stats, manager->widget_y);
}

static gboolean
load_widget_settings(WidgetManager *manager, gboolean *enabled,
                     gint *x, gint *y)
{
    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(settings, manager->settings_path,
                                   G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("Could not read widget settings: %s", error->message);
        g_clear_error(&error);
        g_key_file_unref(settings);
        *enabled = FALSE;
        *x = -1;
        *y = -1;
        return FALSE;
    }
    *enabled = g_key_file_get_boolean(
        settings, "Widgets", "stats_enabled", NULL);
    *x = g_key_file_has_key(settings, "Widgets", "stats_x", NULL)
        ? g_key_file_get_integer(settings, "Widgets", "stats_x", NULL) : -1;
    *y = g_key_file_has_key(settings, "Widgets", "stats_y", NULL)
        ? g_key_file_get_integer(settings, "Widgets", "stats_y", NULL) : -1;
    g_key_file_unref(settings);
    return TRUE;
}

static gboolean
save_widget_position(WidgetManager *manager)
{
    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    if (g_file_test(manager->settings_path, G_FILE_TEST_EXISTS) &&
        !g_key_file_load_from_file(settings, manager->settings_path,
                                   G_KEY_FILE_NONE, &error)) {
        g_warning("Could not load settings before saving widget position: %s",
                  error->message);
        g_clear_error(&error);
        g_key_file_unref(settings);
        return FALSE;
    }
    g_key_file_set_integer(settings, "Widgets", "stats_x", manager->widget_x);
    g_key_file_set_integer(settings, "Widgets", "stats_y", manager->widget_y);
    gchar *contents = g_key_file_to_data(settings, NULL, &error);
    g_key_file_unref(settings);
    if (contents == NULL) {
        g_warning("Could not serialize widget position: %s", error->message);
        g_clear_error(&error);
        return FALSE;
    }
    gchar *directory = g_path_get_dirname(manager->settings_path);
    gboolean saved = g_mkdir_with_parents(directory, 0700) == 0 &&
        g_file_set_contents(manager->settings_path, contents, -1, &error);
    if (!saved) {
        g_warning("Could not save widget position: %s",
                  error ? error->message : "settings directory creation failed");
        g_clear_error(&error);
    }
    g_free(directory);
    g_free(contents);
    return saved;
}

static gboolean
on_stats_button_press(GtkWidget *widget, GdkEventButton *event,
                      gpointer user_data)
{
    WidgetManager *manager = user_data;
    if (event->button != GDK_BUTTON_PRIMARY)
        return FALSE;
    manager->dragging = TRUE;
    manager->drag_x = (gint)event->x_root - manager->widget_x;
    manager->drag_y = (gint)event->y_root - manager->widget_y;
    gtk_widget_grab_focus(widget);
    return TRUE;
}

static gboolean
on_stats_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    WidgetManager *manager = user_data;
    if (!manager->dragging)
        return FALSE;
    manager->widget_x = MAX(0, (gint)event->x_root - manager->drag_x);
    manager->widget_y = MAX(30, (gint)event->y_root - manager->drag_y);
    move_stats_widget(manager, (gint)event->x_root - manager->drag_x,
                      (gint)event->y_root - manager->drag_y);
    return TRUE;
}

static gboolean
on_stats_button_release(GtkWidget *widget, GdkEventButton *event,
                        gpointer user_data)
{
    WidgetManager *manager = user_data;
    (void)widget;
    if (event->button != GDK_BUTTON_PRIMARY || !manager->dragging)
        return FALSE;
    manager->dragging = FALSE;
    save_widget_position(manager);
    return TRUE;
}

static gboolean
sync_widget_preferences(gpointer user_data)
{
    WidgetManager *manager = user_data;
    sync_external_widgets(manager);
    gboolean enabled;
    gint x, y;
    load_widget_settings(manager, &enabled, &x, &y);
    if (!manager->dragging && x >= 0 && y >= 30 &&
        (x != manager->widget_x || y != manager->widget_y)) {
        manager->widget_x = x;
        manager->widget_y = y;
        move_stats_widget(manager, x, y);
    }
    if (enabled != manager->enabled) {
        manager->enabled = enabled;
        set_stats_visible(manager, enabled);
    }
    return G_SOURCE_CONTINUE;
}

static void
free_manager(GtkWidget *widget, gpointer user_data)
{
    WidgetManager *manager = user_data;
    (void)widget;
    if (manager->sync_source != 0)
        g_source_remove(manager->sync_source);
    manager->closing = TRUE;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, manager->external_widgets);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        WidgetProcess *process = value;
        g_subprocess_force_exit(process->child);
    }
    g_hash_table_unref(manager->external_widgets);
    g_free(manager->widgets_path);
    g_free(manager->settings_path);
    g_free(manager);
}

GtkWidget *
macos_de_widget_manager_create(void)
{
    WidgetManager *manager = g_new0(WidgetManager, 1);
    GtkWidget *stats_content = macos_de_stats_widget_create();
    GtkWidget *stats_event_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(stats_event_box), FALSE);
    gtk_container_add(GTK_CONTAINER(stats_event_box), stats_content);
    manager->settings_path = g_build_filename(
        g_get_user_config_dir(), "macos-de", "settings.ini", NULL);
    manager->widgets_path = g_build_filename(
        g_get_user_data_dir(), "macos-de", "widgets", NULL);
    manager->external_widgets = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, widget_process_unref);
    manager->stats = stats_event_box;

    gtk_widget_set_size_request(manager->stats, 250, 144);
    gtk_widget_add_events(manager->stats,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK);
    g_signal_connect(manager->stats, "button-press-event",
                     G_CALLBACK(on_stats_button_press), manager);
    g_signal_connect(manager->stats, "motion-notify-event",
                     G_CALLBACK(on_stats_motion), manager);
    g_signal_connect(manager->stats, "button-release-event",
                     G_CALLBACK(on_stats_button_release), manager);
    gboolean enabled;
    gint x, y;
    load_widget_settings(manager, &enabled, &x, &y);
    manager->enabled = enabled;
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    GdkRectangle geometry = { 0, 0, 1280, 800 };
    if (monitor != NULL)
        gdk_monitor_get_geometry(monitor, &geometry);
    manager->widget_x = x >= 0 ? x : MAX(20, geometry.width - 270);
    manager->widget_y = y >= 30 ? y : 50;
    move_stats_widget(manager, manager->widget_x, manager->widget_y);
    gtk_widget_set_no_show_all(manager->stats, TRUE);
    set_stats_visible(manager, enabled);
    gtk_widget_set_halign(manager->stats, GTK_ALIGN_START);
    gtk_widget_set_valign(manager->stats, GTK_ALIGN_START);
    gtk_widget_set_hexpand(manager->stats, FALSE);
    gtk_widget_set_vexpand(manager->stats, FALSE);
    g_signal_connect(manager->stats, "destroy",
                     G_CALLBACK(free_manager), manager);
    manager->sync_source = g_timeout_add_seconds(
        1, sync_widget_preferences, manager);
    sync_external_widgets(manager);
    return manager->stats;
}
