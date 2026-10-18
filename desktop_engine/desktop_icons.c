#define _GNU_SOURCE
#include "desktop_icons.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <string.h>

enum {
    DESKTOP_ICON_PIXBUF,
    DESKTOP_ICON_NAME,
    DESKTOP_ICON_PATH,
    DESKTOP_ICON_COLUMNS
};

typedef struct {
    GtkListStore *store;
    GtkWidget *view;
    GFileMonitor *monitor;
    gchar *desktop_path;
    gchar *settings_path;
    gchar *context_path;
    gboolean context_is_desktop;
    gboolean right_drag_active;
    gboolean right_drag_moved;
    gint right_drag_start_x;
    gint right_drag_start_y;
    gint right_drag_end_x;
    gint right_drag_end_y;
} DesktopIcons;

static void
free_desktop_icons(gpointer data)
{
    DesktopIcons *desktop = data;
    g_clear_object(&desktop->monitor);
    g_free(desktop->desktop_path);
    g_free(desktop->settings_path);
    g_free(desktop->context_path);
    g_free(desktop);
}

static void
show_desktop_error(const gchar *title, const gchar *message)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                              "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static gchar *
desktop_settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "macos-de", "desktop.ini",
                            NULL);
}

static void refresh_desktop_icons(DesktopIcons *desktop);

static gchar *
find_desktop_icon(const gchar *icon_name)
{
    gchar *executable = g_file_read_link("/proc/self/exe", NULL);
    gchar *binary_directory = executable
        ? g_path_get_dirname(executable) : NULL;
    gchar *build_icons = binary_directory
        ? g_build_filename(binary_directory, "..", "apps", "Easy-File",
                           "icons", icon_name, NULL)
        : NULL;
    gchar *candidates[6];
    guint candidate_count = 0;
    candidates[candidate_count++] =
        g_build_filename("/usr/share/macos-de/apps/Easy-File/icons",
                         icon_name, NULL);
    if (build_icons)
        candidates[candidate_count++] = build_icons;
    candidates[candidate_count++] =
        g_build_filename("build", "apps", "Easy-File", "icons",
                         icon_name, NULL);
    candidates[candidate_count++] =
        g_build_filename("apps", "Easy-File", "icons", icon_name, NULL);
    candidates[candidate_count++] =
        g_build_filename("/usr/share/macos-de/assets/apps", icon_name, NULL);
    candidates[candidate_count++] =
        g_build_filename("assets", "apps", icon_name, NULL);
    gchar *result = NULL;
    for (guint i = 0; i < candidate_count; i++) {
        if (g_file_test(candidates[i], G_FILE_TEST_IS_REGULAR)) {
            result = g_strdup(candidates[i]);
            break;
        }
    }
    for (guint i = 0; i < candidate_count; i++)
        g_free(candidates[i]);
    g_free(binary_directory);
    g_free(executable);
    return result;
}

static const gchar *
desktop_icon_name(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
        return "folder.png";

    gchar *basename = g_path_get_basename(path);
    const gchar *extension = strrchr(basename, '.');
    if (!extension || !extension[1]) {
        g_free(basename);
        return "unknown.png";
    }

    gchar *lower = g_ascii_strdown(extension + 1, -1);
    const gchar *icon = "unknown.png";
    if (g_str_equal(lower, "zip") || g_str_equal(lower, "7z") ||
        g_str_equal(lower, "rar") || g_str_equal(lower, "tar") ||
        g_str_equal(lower, "gz"))
        icon = "zip.png";
    else if (g_str_equal(lower, "c"))
        icon = "c.png";
    else if (g_str_equal(lower, "cc") || g_str_equal(lower, "cpp") ||
             g_str_equal(lower, "cxx") || g_str_equal(lower, "hpp"))
        icon = "c++.png";
    else if (g_str_equal(lower, "cs"))
        icon = "c#.png";
    else if (g_str_equal(lower, "s") || g_str_equal(lower, "asm"))
        icon = "assembly.png";
    else if (g_str_equal(lower, "bin"))
        icon = "bin.png";
    else if (g_str_equal(lower, "deb") || g_str_equal(lower, "rpm") ||
             g_str_equal(lower, "appimage") || g_str_equal(lower, "flatpak") ||
             g_str_equal(lower, "desktop"))
        icon = "software.png";
    else if (g_str_equal(lower, "cfg") || g_str_equal(lower, "conf") ||
             g_str_equal(lower, "ini") || g_str_equal(lower, "toml"))
        icon = "cfg.png";
    else if (g_str_equal(lower, "txt") || g_str_equal(lower, "md") ||
             g_str_equal(lower, "pdf") || g_str_equal(lower, "png") ||
             g_str_equal(lower, "jpg") || g_str_equal(lower, "jpeg") ||
             g_str_equal(lower, "gif") || g_str_equal(lower, "svg") ||
             g_str_equal(lower, "mp3") || g_str_equal(lower, "mp4"))
        icon = "file.png";
    g_free(lower);
    g_free(basename);
    return icon;
}

static gint
compare_desktop_item(gconstpointer left, gconstpointer right, gpointer data)
{
    const gchar *a = *(gchar * const *)left;
    const gchar *b = *(gchar * const *)right;
    GHashTable *positions = data;
    gpointer a_position = g_hash_table_lookup(positions, a);
    gpointer b_position = g_hash_table_lookup(positions, b);
    if (a_position || b_position) {
        guint a_index = a_position ? GPOINTER_TO_UINT(a_position) : G_MAXUINT;
        guint b_index = b_position ? GPOINTER_TO_UINT(b_position) : G_MAXUINT;
        if (a_index != b_index)
            return a_index < b_index ? -1 : 1;
    }
    return g_utf8_collate(a, b);
}

static GdkPixbuf *
load_desktop_icon(const gchar *path)
{
    const gchar *preferred_icon = desktop_icon_name(path);
    gchar *icon_path = find_desktop_icon(preferred_icon);
    GError *error = NULL;
    GdkPixbuf *pixbuf = icon_path
        ? gdk_pixbuf_new_from_file_at_scale(icon_path, 64, 64, TRUE, &error)
        : NULL;
    if (error) {
        g_warning("Could not load desktop icon %s: %s", icon_path,
                  error->message);
        g_clear_error(&error);
    }
    g_free(icon_path);

    if (!pixbuf && !g_str_equal(preferred_icon, "unknown.png")) {
        icon_path = find_desktop_icon("unknown.png");
        if (icon_path)
            pixbuf = gdk_pixbuf_new_from_file_at_scale(
                icon_path, 64, 64, TRUE, &error);
        if (error) {
            g_warning("Could not load fallback desktop icon %s: %s",
                      icon_path ? icon_path : "unknown.png", error->message);
            g_clear_error(&error);
        }
        g_free(icon_path);
    }

    if (!pixbuf)
        g_warning("No macOS-DE desktop icon is available for %s.", path);
    return pixbuf;
}

static gchar *
find_easy_file_executable(void)
{
    gchar *executable = g_file_read_link("/proc/self/exe", NULL);
    gchar *binary_directory = executable
        ? g_path_get_dirname(executable) : NULL;
    gchar *build_candidate = binary_directory
        ? g_build_filename(binary_directory, "..", "apps", "Easy-File",
                           "Easy-File", NULL)
        : NULL;
    const gchar *installed =
        "/usr/share/macos-de/apps/Easy-File/Easy-File";
    gchar *result = NULL;
    if (build_candidate &&
        g_file_test(build_candidate, G_FILE_TEST_IS_EXECUTABLE))
        result = g_strdup(build_candidate);
    else if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE))
        result = g_strdup(installed);
    else if (g_file_test("build/apps/Easy-File/Easy-File",
                         G_FILE_TEST_IS_EXECUTABLE))
        result = g_strdup("build/apps/Easy-File/Easy-File");
    g_free(build_candidate);
    g_free(binary_directory);
    g_free(executable);
    return result;
}

static void
save_desktop_order(DesktopIcons *desktop)
{
    GtkTreeModel *model = GTK_TREE_MODEL(desktop->store);
    GtkTreeIter iter;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gchar *path = NULL;
            gtk_tree_model_get(model, &iter, DESKTOP_ICON_PATH, &path, -1);
            if (path) {
                g_ptr_array_add(names, g_path_get_basename(path));
                g_free(path);
            }
        } while (gtk_tree_model_iter_next(model, &iter));
    }

    gchar *directory = g_path_get_dirname(desktop->settings_path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_warning("Could not create desktop icon settings directory %s.",
                  directory);
    } else {
        GKeyFile *settings = g_key_file_new();
        GError *error = NULL;
        if (g_file_test(desktop->settings_path, G_FILE_TEST_EXISTS) &&
            !g_key_file_load_from_file(settings, desktop->settings_path,
                                       G_KEY_FILE_NONE, &error)) {
            g_warning("Could not read desktop icon settings: %s",
                      error->message);
            g_clear_error(&error);
        }
        g_key_file_set_string_list(
            settings, "Desktop", "order",
            (const gchar * const *)names->pdata, names->len);
        gsize length = 0;
        gchar *contents = g_key_file_to_data(settings, &length, &error);
        if (!contents || !g_file_set_contents(desktop->settings_path,
                                               contents, length, &error)) {
            g_warning("Could not save desktop icon order: %s",
                      error ? error->message : "unknown error");
            g_clear_error(&error);
        }
        g_free(contents);
        g_key_file_unref(settings);
    }
    g_free(directory);
    g_ptr_array_unref(names);
}

static void
on_rows_reordered(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                  gint *new_order, gpointer user_data)
{
    (void)model;
    (void)path;
    (void)iter;
    (void)new_order;
    save_desktop_order(user_data);
}

static void
on_desktop_directory_changed(GFileMonitor *monitor, GFile *file,
                             GFile *other_file, GFileMonitorEvent event,
                             gpointer user_data)
{
    DesktopIcons *desktop = user_data;
    (void)monitor;
    (void)file;
    (void)other_file;
    if (event == G_FILE_MONITOR_EVENT_CREATED ||
        event == G_FILE_MONITOR_EVENT_DELETED ||
        event == G_FILE_MONITOR_EVENT_MOVED ||
        event == G_FILE_MONITOR_EVENT_RENAMED ||
        event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        refresh_desktop_icons(desktop);
}

static void
refresh_desktop_icons(DesktopIcons *desktop)
{
    GError *error = NULL;
    GDir *directory;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GHashTable *positions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    GKeyFile *settings = g_key_file_new();
    gsize order_count = 0;
    gchar **order = NULL;
    if (g_key_file_load_from_file(settings, desktop->settings_path,
                                  G_KEY_FILE_NONE, NULL)) {
        order = g_key_file_get_string_list(settings, "Desktop", "order",
                                           &order_count, NULL);
    }
    for (gsize i = 0; order && i < order_count; i++)
        g_hash_table_insert(positions, g_strdup(order[i]),
                            GUINT_TO_POINTER((guint)i + 1));

    if (g_mkdir_with_parents(desktop->desktop_path, 0755) != 0) {
        gchar *message = g_strdup_printf("Could not access %s.",
                                         desktop->desktop_path);
        show_desktop_error("Desktop folder unavailable", message);
        g_free(message);
        goto done;
    }
    directory = g_dir_open(desktop->desktop_path, 0, &error);
    if (!directory) {
        show_desktop_error("Could not read Desktop",
                           error ? error->message :
                           "The Desktop folder could not be opened.");
        g_clear_error(&error);
        goto done;
    }
    const gchar *name;
    while ((name = g_dir_read_name(directory)) != NULL) {
        if (name[0] == '.')
            continue;
        gchar *path = g_build_filename(desktop->desktop_path, name, NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS) ||
            g_file_test(path, G_FILE_TEST_IS_SYMLINK))
            g_ptr_array_add(names, g_strdup(name));
        g_free(path);
    }
    g_dir_close(directory);
    g_ptr_array_sort_with_data(names, compare_desktop_item, positions);

    gtk_list_store_clear(desktop->store);
    for (guint i = 0; i < names->len; i++) {
        const gchar *entry_name = g_ptr_array_index(names, i);
        gchar *path = g_build_filename(desktop->desktop_path, entry_name, NULL);
        GdkPixbuf *pixbuf = load_desktop_icon(path);
        GtkTreeIter iter;
        gtk_list_store_append(desktop->store, &iter);
        gtk_list_store_set(desktop->store, &iter,
                           DESKTOP_ICON_PIXBUF, pixbuf,
                           DESKTOP_ICON_NAME, entry_name,
                           DESKTOP_ICON_PATH, path, -1);
        g_clear_object(&pixbuf);
        g_free(path);
    }

done:
    g_strfreev(order);
    g_key_file_unref(settings);
    g_hash_table_unref(positions);
    g_ptr_array_unref(names);
}

static gboolean
on_desktop_item_activated(GtkIconView *view, GtkTreePath *path,
                          gpointer user_data)
{
    DesktopIcons *desktop = user_data;
    GtkTreeIter iter;
    gchar *filename = NULL;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(desktop->store), &iter, path))
        return TRUE;
    gtk_tree_model_get(GTK_TREE_MODEL(desktop->store), &iter,
                       DESKTOP_ICON_PATH, &filename, -1);
    if (!filename)
        return TRUE;

    gsize filename_length = strlen(filename);
    if (filename_length >= 9 &&
        g_ascii_strcasecmp(filename + filename_length - 9, ".appimage") == 0) {
        gchar *argv[] = { filename, NULL };
        GError *error = NULL;
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &error)) {
            show_desktop_error("Could not launch AppImage",
                               error ? error->message :
                               "The AppImage could not be started. Check that it is executable.");
            g_clear_error(&error);
        }
        g_free(filename);
        (void)view;
        return TRUE;
    }

    if (g_file_test(filename, G_FILE_TEST_IS_DIR)) {
        gchar *easy_file = find_easy_file_executable();
        if (!easy_file) {
            show_desktop_error("Easy-File unavailable",
                               "Build or install Easy-File to browse folders from the desktop.");
            g_free(filename);
            return TRUE;
        }
        gchar *argv[] = { easy_file, filename, NULL };
        GError *error = NULL;
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &error)) {
            show_desktop_error("Could not open folder in Easy-File",
                               error ? error->message :
                               "Easy-File could not be started.");
            g_clear_error(&error);
        }
        g_free(easy_file);
        g_free(filename);
        (void)view;
        return TRUE;
    }

    GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(filename);
    GError *error = NULL;
    if (app) {
        if (!g_app_info_launch(G_APP_INFO(app), NULL, NULL, &error)) {
            show_desktop_error("Could not launch desktop item",
                               error ? error->message :
                               "The application could not be launched.");
            g_clear_error(&error);
        }
        g_object_unref(app);
    } else {
        GFile *file = g_file_new_for_path(filename);
        gchar *uri = g_file_get_uri(file);
        if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
            show_desktop_error("Could not open desktop item",
                               error ? error->message :
                               "No application can open this item.");
            g_clear_error(&error);
        }
        g_free(uri);
        g_object_unref(file);
    }
    g_free(filename);
    (void)view;
    return TRUE;
}

static void
remove_from_desktop(DesktopIcons *desktop)
{
    GList *selected = gtk_icon_view_get_selected_items(
        GTK_ICON_VIEW(desktop->view));
    for (GList *item = selected; item; item = item->next) {
        GtkTreeIter iter;
        gchar *path = NULL;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(desktop->store), &iter,
                                     item->data))
            continue;
        gtk_tree_model_get(GTK_TREE_MODEL(desktop->store), &iter,
                           DESKTOP_ICON_PATH, &path, -1);
        if (path) {
            GFile *file = g_file_new_for_path(path);
            GError *error = NULL;
            if (!g_file_trash(file, NULL, &error)) {
                show_desktop_error("Could not remove item from Desktop",
                                   error ? error->message :
                                   "The item could not be moved to Trash.");
                g_clear_error(&error);
            }
            g_object_unref(file);
            g_free(path);
        }
    }
    g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    refresh_desktop_icons(desktop);
}

static void
on_desktop_menu_action(GtkMenuItem *item, gpointer user_data)
{
    DesktopIcons *desktop = user_data;
    const gchar *action = g_object_get_data(G_OBJECT(item), "desktop-action");
    if (g_strcmp0(action, "refresh") == 0) {
        refresh_desktop_icons(desktop);
    } else if (g_strcmp0(action, "remove") == 0) {
        remove_from_desktop(desktop);
    } else if (g_strcmp0(action, "wallpaper") == 0 ||
               g_strcmp0(action, "widgets") == 0) {
        gchar *executable = NULL;
        const gchar *installed =
            "/usr/share/macos-de/apps/DE-settings/DE-settings";
        if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE)) {
            executable = g_strdup(installed);
        } else {
            executable = g_build_filename("build", "apps", "DE-settings",
                                         "DE-settings", NULL);
            if (!g_file_test(executable, G_FILE_TEST_IS_EXECUTABLE)) {
                g_free(executable);
                executable = g_find_program_in_path("DE-settings");
            }
        }
        if (!executable) {
            show_desktop_error("Settings unavailable",
                               "Build or install DE Settings to configure the desktop.");
            return;
        }
        gchar *argument = g_strdup_printf("--page=%s", action);
        gchar *argv[] = { executable, argument, NULL };
        GError *error = NULL;
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &error)) {
            show_desktop_error("Could not open Settings",
                               error ? error->message :
                               "DE Settings could not be started.");
            g_clear_error(&error);
        }
        g_free(argument);
        g_free(executable);
    }
}

static gboolean
on_desktop_button_press(GtkWidget *widget, GdkEventButton *event,
                        gpointer user_data)
{
    DesktopIcons *desktop = user_data;
    if (event->type != GDK_BUTTON_PRESS ||
        event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    desktop->right_drag_active = TRUE;
    desktop->right_drag_moved = FALSE;
    desktop->right_drag_start_x = (gint)event->x;
    desktop->right_drag_start_y = (gint)event->y;
    desktop->right_drag_end_x = (gint)event->x;
    desktop->right_drag_end_y = (gint)event->y;
    return TRUE;
}

static gboolean
on_desktop_motion(GtkWidget *widget, GdkEventMotion *event,
                  gpointer user_data)
{
    DesktopIcons *desktop = user_data;
    if (!desktop->right_drag_active)
        return FALSE;
    desktop->right_drag_end_x = (gint)event->x;
    desktop->right_drag_end_y = (gint)event->y;
    if (!desktop->right_drag_moved &&
        (ABS(desktop->right_drag_end_x - desktop->right_drag_start_x) >= 5 ||
         ABS(desktop->right_drag_end_y - desktop->right_drag_start_y) >= 5)) {
        desktop->right_drag_moved = TRUE;
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
    }
    if (desktop->right_drag_moved) {
        gint left = MIN(desktop->right_drag_start_x, desktop->right_drag_end_x);
        gint top = MIN(desktop->right_drag_start_y, desktop->right_drag_end_y);
        GdkRectangle selection = {
            left, top,
            ABS(desktop->right_drag_end_x - desktop->right_drag_start_x),
            ABS(desktop->right_drag_end_y - desktop->right_drag_start_y)
        };
        GtkTreeModel *model = gtk_icon_view_get_model(GTK_ICON_VIEW(widget));
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
        while (valid) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            GdkRectangle item_rect;
            if (gtk_icon_view_get_cell_rect(GTK_ICON_VIEW(widget), path, NULL,
                                            &item_rect) &&
                gdk_rectangle_intersect(&selection, &item_rect, NULL))
                gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
            gtk_tree_path_free(path);
            valid = gtk_tree_model_iter_next(model, &iter);
        }
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

static gboolean
on_desktop_selection_draw(GtkWidget *widget, cairo_t *cr,
                          gpointer user_data)
{
    DesktopIcons *desktop = user_data;
    if (desktop->right_drag_active && desktop->right_drag_moved) {
        gint left = MIN(desktop->right_drag_start_x, desktop->right_drag_end_x);
        gint top = MIN(desktop->right_drag_start_y, desktop->right_drag_end_y);
        double width = ABS(desktop->right_drag_end_x -
                           desktop->right_drag_start_x);
        double height = ABS(desktop->right_drag_end_y -
                            desktop->right_drag_start_y);
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.22, 0.50, 0.92, 0.22);
        cairo_rectangle(cr, left, top, width, height);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.35, 0.65, 1.0, 0.85);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    (void)widget;
    return FALSE;
}

static gboolean
on_desktop_button_release(GtkWidget *widget, GdkEventButton *event,
                          gpointer user_data)
{
    DesktopIcons *desktop = user_data;
    if (event->button != GDK_BUTTON_SECONDARY ||
        !desktop->right_drag_active)
        return FALSE;
    desktop->right_drag_end_x = (gint)event->x;
    desktop->right_drag_end_y = (gint)event->y;
    gboolean dragged = desktop->right_drag_moved;
    desktop->right_drag_active = FALSE;
    desktop->right_drag_moved = FALSE;
    gtk_widget_queue_draw(widget);
    if (dragged)
        return TRUE;

    GtkTreePath *path = gtk_icon_view_get_path_at_pos(
        GTK_ICON_VIEW(widget), (gint)event->x, (gint)event->y);
    GtkWidget *menu = gtk_menu_new();
    if (path) {
        if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), path)) {
            gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
            gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
        }
        gtk_tree_path_free(path);
        GtkWidget *remove = gtk_menu_item_new_with_label("Remove from Desktop");
        g_object_set_data(G_OBJECT(remove), "desktop-action", "remove");
        g_signal_connect(remove, "activate",
                         G_CALLBACK(on_desktop_menu_action), desktop);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), remove);
    } else {
        GtkWidget *refresh = gtk_menu_item_new_with_label("Refresh");
        g_object_set_data(G_OBJECT(refresh), "desktop-action", "refresh");
        g_signal_connect(refresh, "activate",
                         G_CALLBACK(on_desktop_menu_action), desktop);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), refresh);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        GtkWidget *wallpaper = gtk_menu_item_new_with_label("Wallpaper Settings");
        g_object_set_data(G_OBJECT(wallpaper), "desktop-action", "wallpaper");
        g_signal_connect(wallpaper, "activate",
                         G_CALLBACK(on_desktop_menu_action), desktop);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), wallpaper);
        GtkWidget *widgets = gtk_menu_item_new_with_label("Widget Settings");
        g_object_set_data(G_OBJECT(widgets), "desktop-action", "widgets");
        g_signal_connect(widgets, "activate",
                         G_CALLBACK(on_desktop_menu_action), desktop);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), widgets);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    g_signal_connect_swapped(menu, "selection-done",
                             G_CALLBACK(gtk_widget_destroy), menu);
    return TRUE;
}

GtkWidget *
macos_de_desktop_icons_create(void)
{
    DesktopIcons *desktop = g_new0(DesktopIcons, 1);
    desktop->desktop_path = g_build_filename(g_get_home_dir(), "Desktop", NULL);
    desktop->settings_path = desktop_settings_path();
    if (g_mkdir_with_parents(desktop->desktop_path, 0755) == 0) {
        GFile *directory = g_file_new_for_path(desktop->desktop_path);
        GError *monitor_error = NULL;
        desktop->monitor =
            g_file_monitor_directory(directory, G_FILE_MONITOR_NONE, NULL,
                                     &monitor_error);
        if (desktop->monitor)
            g_signal_connect(desktop->monitor, "changed",
                             G_CALLBACK(on_desktop_directory_changed), desktop);
        else {
            g_warning("Could not watch Desktop folder for changes: %s",
                      monitor_error ? monitor_error->message :
                      "unknown file monitor error");
            g_clear_error(&monitor_error);
        }
        g_object_unref(directory);
    }
    desktop->store = gtk_list_store_new(DESKTOP_ICON_COLUMNS, GDK_TYPE_PIXBUF,
                                        G_TYPE_STRING, G_TYPE_STRING);

    GtkWidget *view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(desktop->store));
    desktop->view = view;
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(view), DESKTOP_ICON_PIXBUF);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(view), DESKTOP_ICON_NAME);
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(view), GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(view), 104);
    gtk_icon_view_set_columns(GTK_ICON_VIEW(view), -1);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(view), 12);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(view), 8);
    gtk_icon_view_set_margin(GTK_ICON_VIEW(view), 18);
    gtk_icon_view_set_reorderable(GTK_ICON_VIEW(view), TRUE);
    gtk_widget_add_events(view, GDK_BUTTON_RELEASE_MASK |
                                GDK_POINTER_MOTION_MASK);
    gtk_widget_set_halign(view, GTK_ALIGN_FILL);
    gtk_widget_set_valign(view, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);
    gtk_widget_set_app_paintable(view, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(view),
                                "macos-de-desktop-icons");
    GtkCssProvider *provider = gtk_css_provider_new();
    const gchar css[] =
        "iconview.macos-de-desktop-icons {"
        " background-color: transparent; color: #ffffff;"
        " text-shadow: 0 1px 3px rgba(0,0,0,0.95);"
        "}"
        "iconview.macos-de-desktop-icons cell {"
        " padding: 5px; border-radius: 6px;"
        "}"
        "iconview.macos-de-desktop-icons cell:selected {"
        " background-color: rgba(60,130,230,0.72);"
        " color: #ffffff;"
        "}"
        "iconview.macos-de-desktop-icons cell:selected label {"
        " color: #ffffff;"
        "}";
    GError *css_error = NULL;
    if (!gtk_css_provider_load_from_data(provider, css, -1, &css_error)) {
        g_warning("Could not style desktop icons: %s", css_error->message);
        g_clear_error(&css_error);
    } else {
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(view), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
    g_signal_connect(view, "item-activated",
                     G_CALLBACK(on_desktop_item_activated), desktop);
    g_signal_connect(view, "button-press-event",
                     G_CALLBACK(on_desktop_button_press), desktop);
    g_signal_connect(view, "motion-notify-event",
                     G_CALLBACK(on_desktop_motion), desktop);
    g_signal_connect(view, "button-release-event",
                     G_CALLBACK(on_desktop_button_release), desktop);
    g_signal_connect_after(view, "draw",
                           G_CALLBACK(on_desktop_selection_draw), desktop);
    g_signal_connect(desktop->store, "rows-reordered",
                     G_CALLBACK(on_rows_reordered), desktop);
    g_object_set_data_full(G_OBJECT(view), "desktop-icons-state", desktop,
                           free_desktop_icons);
    g_object_unref(desktop->store);
    refresh_desktop_icons(desktop);
    return view;
}
