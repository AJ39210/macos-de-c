#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

enum {
    COLUMN_ICON,
    COLUMN_LIST_ICON,
    COLUMN_NAME,
    COLUMN_PATH,
    COLUMN_IS_DIRECTORY,
    COLUMN_SIZE,
    N_COLUMNS
};

typedef struct {
    gchar *name;
    gchar *display_name;
    gchar *path;
    gboolean is_directory;
    goffset size;
} FileItem;

typedef struct {
    gchar **uris;
} ClipboardFileList;

typedef struct {
    GtkWidget *window;
    GtkWidget *location_entry;
    GtkWidget *search_entry;
    GtkWidget *hidden_toggle;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *up_button;
    GtkWidget *icon_view;
    GtkWidget *tree_view;
    GtkWidget *stack;
    GtkWidget *status_label;
    GtkWidget *error_bar;
    GtkWidget *error_label;
    GtkListStore *store;
    GtkTreeModelFilter *filter;
    GPtrArray *history;
    gint history_index;
    gchar *current_path;
    gchar *context_path;
    gchar *asset_dir;
    gboolean context_is_directory;
} EasyFile;

typedef enum {
    MENU_OPEN,
    MENU_OPEN_WITH_DEFAULT,
    MENU_COPY_FILES,
    MENU_COPY_PATH,
    MENU_PASTE_FILES,
    MENU_PROPERTIES,
    MENU_RENAME,
    MENU_DELETE,
    MENU_ADD_TO_DESKTOP,
    MENU_NEW_FOLDER,
    MENU_REFRESH
} MenuAction;

static void load_directory(EasyFile *app, const gchar *path, gboolean add_history);
static void refresh_directory(EasyFile *app);
static void open_path(EasyFile *app, const gchar *path, gboolean is_directory);
static void paste_files(EasyFile *app);
static void set_inline_error(EasyFile *app, const gchar *message);
static GPtrArray *selected_paths(EasyFile *app);

static void
free_clipboard_file_list(gpointer data)
{
    ClipboardFileList *files = data;
    g_strfreev(files->uris);
    g_free(files);
}

static void
clear_clipboard_files(GtkClipboard *clipboard, gpointer user_data)
{
    (void)clipboard;
    free_clipboard_file_list(user_data);
}

static void
provide_clipboard_files(GtkClipboard *clipboard, GtkSelectionData *selection,
                        guint info, gpointer user_data)
{
    ClipboardFileList *files = user_data;
    (void)clipboard;
    (void)info;
    gtk_selection_data_set_uris(selection, files->uris);
}

static void
copy_paths_to_clipboard(GPtrArray *paths)
{
    if (paths->len == 0)
        return;
    ClipboardFileList *files = g_new0(ClipboardFileList, 1);
    files->uris = g_new0(gchar *, paths->len + 1);
    for (guint i = 0; i < paths->len; i++) {
        GFile *file = g_file_new_for_path(g_ptr_array_index(paths, i));
        files->uris[i] = g_file_get_uri(file);
        g_object_unref(file);
    }
    GtkTargetEntry target = { "text/uri-list", 0, 0 };
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!gtk_clipboard_set_with_data(clipboard, &target, 1,
                                     provide_clipboard_files,
                                     clear_clipboard_files,
                                     files)) {
        free_clipboard_file_list(files);
        g_warning("Could not place file URIs on the clipboard.");
    } else {
        gtk_clipboard_set_can_store(clipboard, &target, 1);
        gtk_clipboard_store(clipboard);
    }
}

static gboolean
copy_file_recursively(GFile *source, GFile *destination, GError **error)
{
    GFileInfo *info = g_file_query_info(
        source, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (info == NULL)
        return FALSE;

    GFileType type = g_file_info_get_file_type(info);
    gboolean copied = FALSE;
    if (type == G_FILE_TYPE_DIRECTORY) {
        if (!g_file_make_directory(destination, NULL, error)) {
            g_object_unref(info);
            return FALSE;
        }
        GFileEnumerator *enumerator = g_file_enumerate_children(
            source, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
        if (enumerator == NULL) {
            g_object_unref(info);
            return FALSE;
        }
        copied = TRUE;
        while (copied) {
            GFileInfo *child_info = g_file_enumerator_next_file(
                enumerator, NULL, error);
            if (child_info == NULL) {
                if (error != NULL && *error != NULL)
                    copied = FALSE;
                break;
            }
            const gchar *name = g_file_info_get_name(child_info);
            GFile *source_child = g_file_get_child(source, name);
            GFile *destination_child = g_file_get_child(destination, name);
            copied = copy_file_recursively(
                source_child, destination_child, error);
            g_object_unref(destination_child);
            g_object_unref(source_child);
            g_object_unref(child_info);
        }
        if (!g_file_enumerator_close(enumerator, NULL, copied ? error : NULL))
            copied = FALSE;
        g_object_unref(enumerator);
    } else if (type == G_FILE_TYPE_SYMBOLIC_LINK) {
        const gchar *target = g_file_info_get_symlink_target(info);
        if (target == NULL) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "The symbolic link has no target.");
        } else {
            copied = g_file_make_symbolic_link(destination, target, NULL, error);
        }
    } else {
        copied = g_file_copy(source, destination, G_FILE_COPY_ALL_METADATA,
                             NULL, NULL, NULL, error);
    }
    g_object_unref(info);
    return copied;
}

static void
on_clipboard_files_received(GtkClipboard *clipboard,
                            GtkSelectionData *selection, gpointer user_data)
{
    EasyFile *app = user_data;
    gchar **uris = gtk_selection_data_get_uris(selection);
    (void)clipboard;
    if (uris == NULL) {
        set_inline_error(app, "Clipboard does not contain copied files.");
        return;
    }

    GFile *destination_dir = g_file_new_for_path(app->current_path);
    for (guint i = 0; uris[i] != NULL; i++) {
        GFile *source = g_file_new_for_uri(uris[i]);
        gchar *basename = g_file_get_basename(source);
        GFile *destination = basename
            ? g_file_get_child(destination_dir, basename) : NULL;
        GError *error = NULL;
        gboolean source_is_directory = g_file_query_file_type(
            source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) ==
            G_FILE_TYPE_DIRECTORY;
        if (destination == NULL || g_file_equal(source, destination) ||
            g_file_query_exists(destination, NULL) ||
            (source_is_directory && g_file_has_prefix(destination, source))) {
            gchar *message = g_strdup_printf(
                "Could not paste %s: the destination exists or is inside the source folder.",
                basename ? basename : uris[i]);
            set_inline_error(app, message);
            g_free(message);
        } else if (!copy_file_recursively(source, destination, &error)) {
            gchar *message = g_strdup_printf(
                "Could not paste %s: %s", basename ? basename : uris[i],
                error ? error->message : "copy failed.");
            set_inline_error(app, message);
            g_free(message);
            g_clear_error(&error);
        }
        g_clear_object(&destination);
        g_free(basename);
        g_object_unref(source);
    }
    g_object_unref(destination_dir);
    g_strfreev(uris);
    refresh_directory(app);
}

static void
paste_files(EasyFile *app)
{
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    GdkAtom target = gdk_atom_intern_static_string("text/uri-list");
    gtk_clipboard_request_contents(clipboard, target,
                                   on_clipboard_files_received, app);
}

static void
on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                 GtkSelectionData *selection, guint info, guint time,
                 gpointer user_data)
{
    EasyFile *app = user_data;
    GPtrArray *paths = selected_paths(app);
    gchar **uris = g_new0(gchar *, paths->len + 1);
    (void)widget;
    (void)context;
    (void)info;
    (void)time;
    for (guint i = 0; i < paths->len; i++) {
        GFile *file = g_file_new_for_path(g_ptr_array_index(paths, i));
        uris[i] = g_file_get_uri(file);
        g_object_unref(file);
    }
    gtk_selection_data_set_uris(selection, uris);
    g_strfreev(uris);
    g_ptr_array_unref(paths);
}

static void
show_error(EasyFile *app, const gchar *title, const gchar *message)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void
set_inline_error(EasyFile *app, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(app->error_label), message);
    gtk_widget_show(app->error_bar);
}

static void
on_error_bar_response(GtkInfoBar *bar, gint response, gpointer user_data)
{
    (void) response;
    (void) user_data;
    gtk_widget_hide(GTK_WIDGET(bar));
}

static gchar *
find_icon_path(EasyFile *app, const gchar *name)
{
    gchar *path = g_build_filename(app->asset_dir, name, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return path;
    g_free(path);
    return NULL;
}

static const gchar *
icon_name_for_file(const gchar *name, gboolean is_directory)
{
    const gchar *extension;
    gchar *lower;
    const gchar *icon = "unknown.png";
    if (is_directory)
        return "folder.png";

    extension = strrchr(name, '.');
    if (extension == NULL)
        return icon;
    lower = g_ascii_strdown(extension + 1, -1);
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
             g_str_equal(lower, "appimage") || g_str_equal(lower, "flatpak"))
        icon = "software.png";
    else if (g_str_equal(lower, "widg"))
        icon = "widg.png";
    else if (g_str_equal(lower, "cfg") || g_str_equal(lower, "conf") ||
             g_str_equal(lower, "ini") || g_str_equal(lower, "toml"))
        icon = "cfg.png";
    else if (g_str_equal(lower, "png"))
        icon = "png.png";
    else if (g_str_equal(lower, "txt") || g_str_equal(lower, "md") ||
             g_str_equal(lower, "pdf") ||
             g_str_equal(lower, "jpg") || g_str_equal(lower, "jpeg") ||
             g_str_equal(lower, "gif") || g_str_equal(lower, "svg") ||
             g_str_equal(lower, "mp3") || g_str_equal(lower, "mp4"))
        icon = "file.png";
    g_free(lower);
    return icon;
}

static GdkPixbuf *
load_file_icon(EasyFile *app, const gchar *name, gboolean is_directory)
{
    const gchar *icon_name = icon_name_for_file(name, is_directory);
    gchar *icon_path = find_icon_path(app, icon_name);
    GdkPixbuf *pixbuf = NULL;
    GError *error = NULL;
    if (icon_path != NULL)
        pixbuf = gdk_pixbuf_new_from_file_at_scale(icon_path, 64, 64, TRUE, &error);
    g_free(icon_path);
    g_clear_error(&error);

    if (pixbuf == NULL && !g_str_equal(icon_name, "unknown.png")) {
        icon_path = find_icon_path(app, "unknown.png");
        if (icon_path != NULL)
            pixbuf = gdk_pixbuf_new_from_file_at_scale(icon_path, 64, 64, TRUE, &error);
        g_free(icon_path);
        g_clear_error(&error);
    }
    if (pixbuf == NULL) {
        const gchar *theme_icon = is_directory ? "folder" : "text-x-generic";
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        pixbuf = gtk_icon_theme_load_icon(theme, theme_icon, 64, GTK_ICON_LOOKUP_USE_BUILTIN, NULL);
    }
    return pixbuf;
}

static void
file_item_free(gpointer data)
{
    FileItem *item = data;
    g_free(item->name);
    g_free(item->display_name);
    g_free(item->path);
    g_free(item);
}

static gint
compare_file_items(gconstpointer left, gconstpointer right)
{
    const FileItem *a = *(FileItem * const *) left;
    const FileItem *b = *(FileItem * const *) right;
    if (a->is_directory != b->is_directory)
        return a->is_directory ? -1 : 1;
    return g_utf8_collate(a->display_name, b->display_name);
}

static gboolean
filter_row_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
    EasyFile *app = user_data;
    gchar *name = NULL;
    gchar *folded_name;
    gchar *folded_query;
    gboolean visible = TRUE;
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(app->search_entry));

    gtk_tree_model_get(model, iter, COLUMN_NAME, &name, -1);
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->hidden_toggle)) &&
        g_str_has_prefix(name, "."))
        visible = FALSE;
    if (visible && query[0] != '\0') {
        folded_name = g_utf8_casefold(name, -1);
        folded_query = g_utf8_casefold(query, -1);
        visible = strstr(folded_name, folded_query) != NULL;
        g_free(folded_name);
        g_free(folded_query);
    }
    g_free(name);
    return visible;
}

static gchar *
format_location_title(const gchar *path)
{
    const gchar *home = g_get_home_dir();
    if (g_strcmp0(path, home) == 0)
        return g_strdup("Home");
    if (g_str_has_prefix(path, home) && path[strlen(home)] == G_DIR_SEPARATOR)
        return g_strdup(path + strlen(home) + 1);
    if (g_str_equal(path, G_DIR_SEPARATOR_S))
        return g_strdup("Filesystem");
    return g_path_get_basename(path);
}

static void
update_navigation_controls(EasyFile *app)
{
    gtk_widget_set_sensitive(app->back_button, app->history_index > 0);
    gtk_widget_set_sensitive(app->forward_button,
        app->history_index >= 0 && app->history_index + 1 < (gint) app->history->len);
    gtk_widget_set_sensitive(app->up_button,
        app->current_path != NULL && !g_str_equal(app->current_path, G_DIR_SEPARATOR_S));
}

static void
update_status(EasyFile *app)
{
    GtkTreeModel *model = GTK_TREE_MODEL(app->filter);
    gint count = gtk_tree_model_iter_n_children(model, NULL);
    gchar *text = g_strdup_printf("%d item%s", count, count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(app->status_label), text);
    g_free(text);
}

static void
refresh_directory(EasyFile *app)
{
    GFile *directory;
    GFileEnumerator *enumerator;
    GError *error = NULL;
    GPtrArray *items = g_ptr_array_new_with_free_func(file_item_free);
    GFileInfo *info;

    gtk_list_store_clear(app->store);
    directory = g_file_new_for_path(app->current_path);
    enumerator = g_file_enumerate_children(
        directory, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (enumerator == NULL) {
        set_inline_error(app, error != NULL ? error->message : "Unable to read this folder.");
        g_clear_error(&error);
        g_object_unref(directory);
        g_ptr_array_unref(items);
        update_status(app);
        return;
    }

    while ((info = g_file_enumerator_next_file(enumerator, NULL, &error)) != NULL) {
        const gchar *name = g_file_info_get_name(info);
        FileItem *item = g_new0(FileItem, 1);
        item->name = g_strdup(name);
        item->display_name = g_filename_display_name(name);
        item->path = g_build_filename(app->current_path, name, NULL);
        item->is_directory = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        item->size = g_file_info_get_size(info);
        g_ptr_array_add(items, item);
        g_object_unref(info);
    }

    if (error != NULL) {
        set_inline_error(app, error->message);
        g_clear_error(&error);
    } else {
        gtk_widget_hide(app->error_bar);
    }
    g_file_enumerator_close(enumerator, NULL, NULL);
    g_object_unref(enumerator);
    g_object_unref(directory);

    g_ptr_array_sort(items, compare_file_items);
    for (guint i = 0; i < items->len; i++) {
        FileItem *item = g_ptr_array_index(items, i);
        GdkPixbuf *icon = load_file_icon(app, item->name, item->is_directory);
        GdkPixbuf *list_icon = icon
            ? gdk_pixbuf_scale_simple(icon, 32, 32, GDK_INTERP_BILINEAR)
            : NULL;
        gchar *size = item->is_directory ? g_strdup("—") :
                      g_format_size(item->size);
        GtkTreeIter iter;
        gtk_list_store_append(app->store, &iter);
        gtk_list_store_set(app->store, &iter,
            COLUMN_ICON, icon,
            COLUMN_LIST_ICON, list_icon,
            COLUMN_NAME, item->display_name,
            COLUMN_PATH, item->path,
            COLUMN_IS_DIRECTORY, item->is_directory,
            COLUMN_SIZE, size, -1);
        g_clear_object(&icon);
        g_clear_object(&list_icon);
        g_free(size);
    }
    g_ptr_array_unref(items);
    gtk_tree_model_filter_refilter(app->filter);
    update_status(app);
}

static void
load_directory(EasyFile *app, const gchar *path, gboolean add_history)
{
    gchar *canonical = g_canonicalize_filename(path, NULL);
    gchar *title;
    if (!g_file_test(canonical, G_FILE_TEST_IS_DIR)) {
        gchar *message = g_strdup_printf("“%s” is not an accessible folder.", canonical);
        set_inline_error(app, message);
        g_free(message);
        g_free(canonical);
        return;
    }

    if (add_history) {
        while ((gint) app->history->len > app->history_index + 1)
            g_ptr_array_remove_index(app->history, app->history->len - 1);
        g_ptr_array_add(app->history, g_strdup(canonical));
        app->history_index = (gint) app->history->len - 1;
    }
    g_free(app->current_path);
    app->current_path = canonical;
    gtk_entry_set_text(GTK_ENTRY(app->location_entry), app->current_path);
    title = format_location_title(app->current_path);
    gchar *window_title = g_strdup_printf("%s — Easy File", title);
    gtk_window_set_title(GTK_WINDOW(app->window), window_title);
    g_free(window_title);
    g_free(title);
    update_navigation_controls(app);
    refresh_directory(app);
}

static void
navigate_history(EasyFile *app, gint offset)
{
    gint target = app->history_index + offset;
    if (target < 0 || target >= (gint) app->history->len)
        return;
    app->history_index = target;
    update_navigation_controls(app);
    load_directory(app, g_ptr_array_index(app->history, target), FALSE);
}

static void
open_path(EasyFile *app, const gchar *path, gboolean is_directory)
{
    if (is_directory) {
        load_directory(app, path, TRUE);
        return;
    }
    gsize path_length = strlen(path);
    if (path_length >= 9 &&
        g_ascii_strcasecmp(path + path_length - 9, ".appimage") == 0) {
        gchar *argv[] = { (gchar *)path, NULL };
        GError *error = NULL;
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &error)) {
            show_error(app, "Could not launch AppImage",
                       error ? error->message :
                       "The AppImage could not be started. Check that it is executable.");
            g_clear_error(&error);
        }
        return;
    }
    GFile *file = g_file_new_for_path(path);
    gchar *uri = g_file_get_uri(file);
    GError *error = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
        show_error(app, "Could not open file",
                   error != NULL ? error->message : "No application can open this file.");
        g_clear_error(&error);
    }
    g_free(uri);
    g_object_unref(file);
}

static void
on_location_activate(GtkEntry *entry, gpointer user_data)
{
    EasyFile *app = user_data;
    const gchar *text = gtk_entry_get_text(entry);
    gchar *path;
    if (g_str_equal(text, "~"))
        path = g_strdup(g_get_home_dir());
    else if (g_str_has_prefix(text, "~/"))
        path = g_build_filename(g_get_home_dir(), text + 2, NULL);
    else
        path = g_strdup(text);
    load_directory(app, path, TRUE);
    g_free(path);
}

static void
on_search_changed(GtkEditable *editable, gpointer user_data)
{
    EasyFile *app = user_data;
    (void) editable;
    gtk_tree_model_filter_refilter(app->filter);
    update_status(app);
}

static void
on_hidden_toggled(GtkToggleButton *button, gpointer user_data)
{
    EasyFile *app = user_data;
    (void) button;
    gtk_tree_model_filter_refilter(app->filter);
    update_status(app);
}

static void
on_icon_activated(GtkIconView *view, GtkTreePath *path, gpointer user_data)
{
    EasyFile *app = user_data;
    GtkTreeIter iter;
    gchar *file_path = NULL;
    gboolean is_directory = FALSE;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->filter), &iter, path)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->filter), &iter,
                           COLUMN_PATH, &file_path,
                           COLUMN_IS_DIRECTORY, &is_directory, -1);
        open_path(app, file_path, is_directory);
    }
    g_free(file_path);
    (void) view;
}

static void
on_tree_row_activated(GtkTreeView *view, GtkTreePath *path,
                      GtkTreeViewColumn *column, gpointer user_data)
{
    EasyFile *app = user_data;
    GtkTreeIter iter;
    gchar *file_path = NULL;
    gboolean is_directory = FALSE;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->filter), &iter, path)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->filter), &iter,
                           COLUMN_PATH, &file_path,
                           COLUMN_IS_DIRECTORY, &is_directory, -1);
        open_path(app, file_path, is_directory);
    }
    g_free(file_path);
    (void) view;
    (void) column;
}

static gchar *
selected_path(EasyFile *app, gboolean *is_directory)
{
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    gchar *path = NULL;
    gboolean directory = FALSE;
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(app->stack)), "grid") == 0) {
        GList *selection = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app->icon_view));
        if (selection != NULL) {
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->filter), &iter, selection->data))
                gtk_tree_model_get(GTK_TREE_MODEL(app->filter), &iter,
                                   COLUMN_PATH, &path,
                                   COLUMN_IS_DIRECTORY, &directory, -1);
            g_list_free_full(selection, (GDestroyNotify) gtk_tree_path_free);
        }
    } else {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
        if (gtk_tree_selection_get_selected(selection, &model, &iter))
            gtk_tree_model_get(model, &iter, COLUMN_PATH, &path,
                               COLUMN_IS_DIRECTORY, &directory, -1);
    }
    if (is_directory != NULL)
        *is_directory = directory;
    return path;
}

static GPtrArray *
selected_paths(EasyFile *app)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(app->stack)),
                  "grid") == 0) {
        GList *selection =
            gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app->icon_view));
        for (GList *item = selection; item; item = item->next) {
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->filter), &iter,
                                        item->data)) {
                gchar *path = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(app->filter), &iter,
                                   COLUMN_PATH, &path, -1);
                if (path)
                    g_ptr_array_add(paths, path);
            }
        }
        g_list_free_full(selection, (GDestroyNotify)gtk_tree_path_free);
    } else {
        GtkTreeSelection *selection =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
        GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
        for (GList *item = rows; item; item = item->next) {
            if (gtk_tree_model_get_iter(model, &iter, item->data)) {
                gchar *path = NULL;
                gtk_tree_model_get(model, &iter, COLUMN_PATH, &path, -1);
                if (path)
                    g_ptr_array_add(paths, path);
            }
        }
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    }
    return paths;
}

static void
delete_selected(EasyFile *app)
{
    GPtrArray *paths = selected_paths(app);
    if (paths->len == 0) {
        g_ptr_array_unref(paths);
        return;
    }
    gchar *prompt = g_strdup_printf(
        paths->len == 1 ? "Move this item to Trash?" :
                          "Move these %u items to Trash?",
        paths->len);
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", prompt);
    g_free(prompt);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Move to Trash",
                          GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response == GTK_RESPONSE_ACCEPT) {
        for (guint i = 0; i < paths->len; i++) {
            GFile *file = g_file_new_for_path(g_ptr_array_index(paths, i));
            GError *error = NULL;
            if (!g_file_trash(file, NULL, &error)) {
                show_error(app, "Could not move item to Trash",
                           error ? error->message : "The item could not be trashed.");
                g_clear_error(&error);
            }
            g_object_unref(file);
        }
        refresh_directory(app);
    }
    g_ptr_array_unref(paths);
}

static void
rename_selected(EasyFile *app, const gchar *path)
{
    GFile *file = g_file_new_for_path(path);
    gchar *basename = g_file_get_basename(file);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Rename", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Rename", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), basename ? basename : "");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_container_set_border_width(
        GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *new_name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (!*new_name || strchr(new_name, G_DIR_SEPARATOR) ||
            g_str_equal(new_name, ".") || g_str_equal(new_name, "..")) {
            show_error(app, "Invalid name",
                       "Enter a non-empty name without path separators.");
        } else {
            GError *error = NULL;
            GFile *renamed = g_file_set_display_name(
                file, new_name, NULL, &error);
            if (!renamed) {
                show_error(app, "Could not rename item",
                           error ? error->message : "The rename operation failed.");
                g_clear_error(&error);
            } else {
                g_object_unref(renamed);
                refresh_directory(app);
            }
        }
    }
    gtk_widget_destroy(dialog);
    g_free(basename);
    g_object_unref(file);
}

static void
add_to_desktop(EasyFile *app, const gchar *path)
{
    gchar *desktop = g_build_filename(g_get_home_dir(), "Desktop", NULL);
    if (g_mkdir_with_parents(desktop, 0755) != 0) {
        gchar *message = g_strdup_printf("Could not create %s.", desktop);
        show_error(app, "Could not add item to Desktop", message);
        g_free(message);
        g_free(desktop);
        return;
    }
    gchar *name = g_path_get_basename(path);
    gchar *link_path = g_build_filename(desktop, name, NULL);
    if (g_strcmp0(path, link_path) == 0) {
        show_error(app, "Already on Desktop",
                   "This item is already in the Desktop folder.");
    } else {
        GFile *link = g_file_new_for_path(link_path);
        GError *error = NULL;
        if (!g_file_make_symbolic_link(link, path, NULL, &error)) {
            show_error(app, "Could not add item to Desktop",
                       error ? error->message :
                       "A desktop shortcut could not be created.");
            g_clear_error(&error);
        }
        g_object_unref(link);
    }
    g_free(link_path);
    g_free(name);
    g_free(desktop);
}

static void
show_properties(EasyFile *app, const gchar *path)
{
    GFile *file = g_file_new_for_path(path);
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (info == NULL) {
        show_error(app, "Could not read properties",
                   error != NULL ? error->message : "File information is unavailable.");
        g_clear_error(&error);
        g_object_unref(file);
        return;
    }
    gboolean is_directory = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    gchar *size = is_directory ? g_strdup("Folder") :
                  g_format_size(g_file_info_get_size(info));
    gchar *basename = g_file_get_basename(file);
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", basename);
    gchar *detail = g_strdup_printf("%s\n%s\n%s", is_directory ? "Folder" : "File", path, size);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(detail);
    g_free(basename);
    g_free(size);
    g_object_unref(info);
    g_object_unref(file);
}

static void
create_folder(EasyFile *app)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Create Folder", GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Folder name");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (name[0] == '\0' || strchr(name, G_DIR_SEPARATOR) != NULL ||
            g_str_equal(name, ".") || g_str_equal(name, "..")) {
            show_error(app, "Invalid folder name", "Enter a simple, non-empty folder name.");
        } else {
            gchar *path = g_build_filename(app->current_path, name, NULL);
            GFile *folder = g_file_new_for_path(path);
            GError *error = NULL;
            if (!g_file_make_directory(folder, NULL, &error)) {
                show_error(app, "Could not create folder",
                           error != NULL ? error->message : "The folder could not be created.");
                g_clear_error(&error);
            } else {
                refresh_directory(app);
            }
            g_object_unref(folder);
            g_free(path);
        }
    }
    gtk_widget_destroy(dialog);
}

static void
on_context_action(GtkMenuItem *item, gpointer user_data)
{
    EasyFile *app = user_data;
    MenuAction action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "easy-file-action"));
    if (action == MENU_NEW_FOLDER) {
        create_folder(app);
    } else if (action == MENU_REFRESH) {
        refresh_directory(app);
    } else if (action == MENU_COPY_FILES) {
        GPtrArray *paths = selected_paths(app);
        copy_paths_to_clipboard(paths);
        g_ptr_array_unref(paths);
    } else if (action == MENU_PASTE_FILES) {
        paste_files(app);
    } else if (action == MENU_DELETE) {
        delete_selected(app);
    } else if (action == MENU_ADD_TO_DESKTOP) {
        GPtrArray *paths = selected_paths(app);
        for (guint i = 0; i < paths->len; i++)
            add_to_desktop(app, g_ptr_array_index(paths, i));
        g_ptr_array_unref(paths);
    } else if (app->context_path != NULL) {
        if (action == MENU_OPEN || action == MENU_OPEN_WITH_DEFAULT)
            open_path(app, app->context_path, app->context_is_directory);
        else if (action == MENU_COPY_PATH)
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                                   app->context_path, -1);
        else if (action == MENU_PROPERTIES)
            show_properties(app, app->context_path);
        else if (action == MENU_RENAME)
            rename_selected(app, app->context_path);
    }
}

static void
add_context_item(GtkWidget *menu, const gchar *label, MenuAction action, EasyFile *app)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(item), "easy-file-action", GINT_TO_POINTER(action));
    g_signal_connect(item, "activate", G_CALLBACK(on_context_action), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void
show_context_menu(EasyFile *app, GdkEventButton *event, gboolean has_item)
{
    GtkWidget *menu = gtk_menu_new();
    g_free(app->context_path);
    app->context_path = has_item ? selected_path(app, &app->context_is_directory) : NULL;
    if (app->context_path != NULL) {
        add_context_item(menu, app->context_is_directory ? "Open" : "Open with Default App",
                         MENU_OPEN, app);
        add_context_item(menu, "Copy", MENU_COPY_FILES, app);
        add_context_item(menu, "Copy Path", MENU_COPY_PATH, app);
        add_context_item(menu, "Properties", MENU_PROPERTIES, app);
        GPtrArray *selection = selected_paths(app);
        if (selection->len == 1) {
            add_context_item(menu, "Rename…", MENU_RENAME, app);
            add_context_item(menu, "Add to Desktop", MENU_ADD_TO_DESKTOP, app);
        }
        if (selection->len > 1)
            add_context_item(menu, "Add to Desktop", MENU_ADD_TO_DESKTOP, app);
        g_ptr_array_unref(selection);
        add_context_item(menu, "Delete", MENU_DELETE, app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }
    add_context_item(menu, "Paste", MENU_PASTE_FILES, app);
    add_context_item(menu, "New Folder…", MENU_NEW_FOLDER, app);
    add_context_item(menu, "Refresh", MENU_REFRESH, app);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) event);
    g_signal_connect_swapped(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), menu);
}

static gboolean
on_view_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)user_data;
    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    if (GTK_IS_ICON_VIEW(widget)) {
        GtkTreePath *path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget),
                                                          (gint) event->x, (gint) event->y);
        if (path != NULL) {
            if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), path))
                gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
            gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
            gtk_tree_path_free(path);
        }
    } else {
        GtkTreePath *path = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint) event->x,
                                          (gint) event->y, &path, NULL, NULL, NULL)) {
            GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
            if (!gtk_tree_selection_path_is_selected(selection, path))
                gtk_tree_selection_unselect_all(selection);
            gtk_tree_selection_select_path(selection, path);
            gtk_tree_path_free(path);
        }
    }
    return FALSE;
}

static gboolean
on_view_button_release(GtkWidget *widget, GdkEventButton *event,
                       gpointer user_data)
{
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    gboolean has_item;
    if (GTK_IS_ICON_VIEW(widget)) {
        GtkTreePath *path = gtk_icon_view_get_path_at_pos(
            GTK_ICON_VIEW(widget), (gint)event->x, (gint)event->y);
        has_item = path != NULL;
        if (path)
            gtk_tree_path_free(path);
    } else {
        GtkTreePath *path = NULL;
        has_item = gtk_tree_view_get_path_at_pos(
            GTK_TREE_VIEW(widget), (gint)event->x, (gint)event->y,
            &path, NULL, NULL, NULL);
        if (path)
            gtk_tree_path_free(path);
    }
    show_context_menu(user_data, event, has_item);
    return TRUE;
}

static void
on_back_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    navigate_history(user_data, -1);
}

static void
on_forward_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    navigate_history(user_data, 1);
}

static void
on_up_clicked(GtkButton *button, gpointer user_data)
{
    EasyFile *app = user_data;
    gchar *parent = g_path_get_dirname(app->current_path);
    load_directory(app, parent, TRUE);
    g_free(parent);
    (void) button;
}

static void
on_home_clicked(GtkButton *button, gpointer user_data)
{
    load_directory(user_data, g_get_home_dir(), TRUE);
    (void) button;
}

static void
on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    refresh_directory(user_data);
    (void) button;
}

static void
on_view_toggled(GtkToggleButton *button, gpointer user_data)
{
    EasyFile *app = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(button), "easy-file-view");
    if (gtk_toggle_button_get_active(button) && name != NULL)
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), name);
}

static void
on_icon_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation,
                           gpointer user_data)
{
    (void) user_data;
    gint columns = MAX(1, (allocation->width - 24) / 168);
    if (gtk_icon_view_get_columns(GTK_ICON_VIEW(widget)) != columns)
        gtk_icon_view_set_columns(GTK_ICON_VIEW(widget), columns);
}

static void
on_shortcut_selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    EasyFile *app = user_data;
    const gchar *path;
    (void) list;
    if (row == NULL)
        return;
    path = g_object_get_data(G_OBJECT(row), "easy-file-path");
    if (path != NULL)
        load_directory(app, path, TRUE);
}

static void
add_shortcut(GtkWidget *list, const gchar *label, const gchar *icon, const gchar *path)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    GtkWidget *name = gtk_label_new(label);
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 7);
    gtk_widget_set_margin_bottom(box, 7);
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), name, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data_full(G_OBJECT(row), "easy-file-path", g_strdup(path), g_free);
    gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
}

static GtkWidget *
make_toolbar_button(const gchar *icon, const gchar *tooltip)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

static gboolean
on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    EasyFile *app = user_data;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app->window));
    gboolean file_view_has_focus =
        focus == app->icon_view || focus == app->tree_view ||
        widget == app->icon_view || widget == app->tree_view;
    if (file_view_has_focus &&
        (event->state & GDK_CONTROL_MASK) &&
        (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C)) {
        GPtrArray *paths = selected_paths(app);
        copy_paths_to_clipboard(paths);
        g_ptr_array_unref(paths);
        return TRUE;
    }
    if (file_view_has_focus &&
        (event->state & GDK_CONTROL_MASK) &&
        (event->keyval == GDK_KEY_v || event->keyval == GDK_KEY_V)) {
        paste_files(app);
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_l) {
        gtk_widget_grab_focus(app->location_entry);
        gtk_editable_select_region(GTK_EDITABLE(app->location_entry), 0, -1);
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_f) {
        gtk_widget_grab_focus(app->search_entry);
        gtk_editable_select_region(GTK_EDITABLE(app->search_entry), 0, -1);
        return TRUE;
    }
    if ((event->state & GDK_MOD1_MASK) && event->keyval == GDK_KEY_Left) {
        navigate_history(app, -1);
        return TRUE;
    }
    if ((event->state & GDK_MOD1_MASK) && event->keyval == GDK_KEY_Right) {
        navigate_history(app, 1);
        return TRUE;
    }
    if ((event->state & GDK_MOD1_MASK) && event->keyval == GDK_KEY_Up) {
        gchar *parent = g_path_get_dirname(app->current_path);
        load_directory(app, parent, TRUE);
        g_free(parent);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_F5) {
        refresh_directory(app);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Delete ||
        event->keyval == GDK_KEY_KP_Delete) {
        delete_selected(app);
        return TRUE;
    }
    (void) widget;
    return FALSE;
}

static void
apply_css(GtkWidget *window, const gchar *argv0)
{
    gchar *directory = g_path_get_dirname(argv0);
    gchar *css_path = g_build_filename(directory, "Easy-File.css", NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (gtk_css_provider_load_from_path(provider, css_path, &error))
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    else {
        g_warning("Could not load Easy File styling: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(provider);
    g_free(css_path);
    g_free(directory);
    (void) window;
}

static void
setup_views(EasyFile *app)
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    static GtkTargetEntry drag_targets[] = {
        { "text/uri-list", 0, 0 }
    };
    GtkWidget *icon_scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(icon_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    app->icon_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(app->filter));
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(app->icon_view), COLUMN_ICON);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(app->icon_view), COLUMN_NAME);
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(app->icon_view), GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(app->icon_view), 156);
    gtk_icon_view_set_columns(GTK_ICON_VIEW(app->icon_view), 4);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(app->icon_view), 18);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(app->icon_view), 12);
    gtk_icon_view_set_margin(GTK_ICON_VIEW(app->icon_view), 18);
    gtk_widget_set_halign(app->icon_view, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(app->icon_view, TRUE);
    g_signal_connect(app->icon_view, "size-allocate",
                     G_CALLBACK(on_icon_view_size_allocate), app);
    gtk_container_add(GTK_CONTAINER(icon_scroll), app->icon_view);
    g_signal_connect(app->icon_view, "item-activated", G_CALLBACK(on_icon_activated), app);
    g_signal_connect(app->icon_view, "button-press-event", G_CALLBACK(on_view_button_press), app);
    g_signal_connect(app->icon_view, "button-release-event",
                     G_CALLBACK(on_view_button_release), app);
    gtk_drag_source_set(app->icon_view,
                        GDK_BUTTON1_MASK | GDK_BUTTON3_MASK,
                        drag_targets, G_N_ELEMENTS(drag_targets),
                        GDK_ACTION_COPY);
    g_signal_connect(app->icon_view, "drag-data-get",
                     G_CALLBACK(on_drag_data_get), app);
    g_signal_connect(app->icon_view, "key-press-event",
                     G_CALLBACK(on_key_press), app);
    gtk_stack_add_named(GTK_STACK(app->stack), icon_scroll, "grid");

    app->tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->filter));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->tree_view), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(app->tree_view), TRUE);
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view)),
        GTK_SELECTION_MULTIPLE);
    renderer = gtk_cell_renderer_pixbuf_new();
    column = gtk_tree_view_column_new_with_attributes(
        "", renderer, "pixbuf", COLUMN_LIST_ICON, NULL);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, 48);
    gtk_cell_renderer_set_fixed_size(renderer, 36, 36);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree_view), column);
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Name", renderer, "text", COLUMN_NAME, NULL);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree_view), column);
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Size", renderer, "text", COLUMN_SIZE, NULL);
    gtk_tree_view_column_set_min_width(column, 100);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree_view), column);
    gtk_container_add(GTK_CONTAINER(list_scroll), app->tree_view);
    g_signal_connect(app->tree_view, "row-activated", G_CALLBACK(on_tree_row_activated), app);
    g_signal_connect(app->tree_view, "button-press-event", G_CALLBACK(on_view_button_press), app);
    g_signal_connect(app->tree_view, "button-release-event",
                     G_CALLBACK(on_view_button_release), app);
    gtk_drag_source_set(app->tree_view,
                        GDK_BUTTON1_MASK | GDK_BUTTON3_MASK,
                        drag_targets, G_N_ELEMENTS(drag_targets),
                        GDK_ACTION_COPY);
    g_signal_connect(app->tree_view, "drag-data-get",
                     G_CALLBACK(on_drag_data_get), app);
    g_signal_connect(app->tree_view, "key-press-event",
                     G_CALLBACK(on_key_press), app);
    gtk_stack_add_named(GTK_STACK(app->stack), list_scroll, "list");
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "grid");
}

static void
build_ui(EasyFile *app)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    GtkWidget *body = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *sidebar_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *sidebar = gtk_list_box_new();
    GtkWidget *sidebar_title = gtk_label_new("FAVORITES");
    GtkWidget *view_switch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *grid_button = gtk_radio_button_new(NULL);
    GtkWidget *list_button = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(grid_button));
    GtkWidget *main_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app->back_button = make_toolbar_button("go-previous-symbolic", "Back (Alt+Left)");
    app->forward_button = make_toolbar_button("go-next-symbolic", "Forward (Alt+Right)");
    app->up_button = make_toolbar_button("go-up-symbolic", "Parent folder (Alt+Up)");
    GtkWidget *home_button = make_toolbar_button("go-home-symbolic", "Home");
    GtkWidget *refresh_button = make_toolbar_button("view-refresh-symbolic", "Refresh (F5)");
    g_signal_connect(app->back_button, "clicked", G_CALLBACK(on_back_clicked), app);
    g_signal_connect(app->forward_button, "clicked", G_CALLBACK(on_forward_clicked), app);
    g_signal_connect(app->up_button, "clicked", G_CALLBACK(on_up_clicked), app);
    g_signal_connect(home_button, "clicked", G_CALLBACK(on_home_clicked), app);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->up_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), home_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 5);

    app->location_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->location_entry), "Enter a folder path");
    gtk_widget_set_hexpand(app->location_entry, TRUE);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(app->location_entry),
                                     GTK_ENTRY_ICON_PRIMARY, "folder-symbolic");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(app->location_entry),
                                    GTK_ENTRY_ICON_PRIMARY, "Current folder");
    g_signal_connect(app->location_entry, "activate", G_CALLBACK(on_location_activate), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->location_entry, TRUE, TRUE, 4);

    app->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->search_entry), "Search this folder");
    gtk_widget_set_size_request(app->search_entry, 190, -1);
    g_signal_connect(app->search_entry, "search-changed", G_CALLBACK(on_search_changed), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->search_entry, FALSE, FALSE, 0);

    app->hidden_toggle = gtk_toggle_button_new_with_label("Hidden");
    gtk_widget_set_tooltip_text(app->hidden_toggle, "Show hidden files");
    g_signal_connect(app->hidden_toggle, "toggled", G_CALLBACK(on_hidden_toggled), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->hidden_toggle, FALSE, FALSE, 0);

    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(grid_button), FALSE);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(list_button), FALSE);
    gtk_button_set_image(GTK_BUTTON(grid_button),
                         gtk_image_new_from_icon_name("view-grid-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_image(GTK_BUTTON(list_button),
                         gtk_image_new_from_icon_name("view-list-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(grid_button), TRUE);
    gtk_widget_set_tooltip_text(grid_button, "Icon grid");
    gtk_widget_set_tooltip_text(list_button, "List");
    g_object_set_data(G_OBJECT(grid_button), "easy-file-view", "grid");
    g_object_set_data(G_OBJECT(list_button), "easy-file-view", "list");
    gtk_box_pack_start(GTK_BOX(view_switch), grid_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view_switch), list_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), view_switch, FALSE, FALSE, 0);
    g_signal_connect(grid_button, "toggled", G_CALLBACK(on_view_toggled), app);
    g_signal_connect(list_button, "toggled", G_CALLBACK(on_view_toggled), app);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "easy-file-toolbar");

    gtk_widget_set_name(sidebar_frame, "easy-file-sidebar");
    gtk_widget_set_size_request(sidebar_frame, 194, -1);
    gtk_widget_set_hexpand(sidebar_frame, FALSE);
    gtk_widget_set_margin_start(sidebar_title, 15);
    gtk_widget_set_margin_top(sidebar_title, 16);
    gtk_widget_set_margin_bottom(sidebar_title, 5);
    gtk_widget_set_halign(sidebar_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_title), "sidebar-heading");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(sidebar), TRUE);
    add_shortcut(sidebar, "Home", "user-home-symbolic", g_get_home_dir());
    gchar *common_path = g_build_filename(g_get_home_dir(), "Desktop", NULL);
    add_shortcut(sidebar, "Desktop", "user-desktop-symbolic", common_path);
    g_free(common_path);
    common_path = g_build_filename(g_get_home_dir(), "Documents", NULL);
    add_shortcut(sidebar, "Documents", "folder-documents-symbolic", common_path);
    g_free(common_path);
    common_path = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    add_shortcut(sidebar, "Downloads", "folder-download-symbolic", common_path);
    g_free(common_path);
    add_shortcut(sidebar, "Filesystem", "drive-harddisk-symbolic", G_DIR_SEPARATOR_S);
    gtk_box_pack_start(GTK_BOX(sidebar_frame), sidebar_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_frame), sidebar, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(body), sidebar_frame, FALSE, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(body), "easy-file-body");
    g_signal_connect(sidebar, "row-activated", G_CALLBACK(on_shortcut_selected), app);

    app->error_bar = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(app->error_bar), GTK_MESSAGE_ERROR);
    gtk_info_bar_add_button(GTK_INFO_BAR(app->error_bar), "Dismiss", GTK_RESPONSE_CLOSE);
    app->error_label = gtk_label_new("");
    gtk_widget_set_halign(app->error_label, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(app->error_bar))),
                      app->error_label);
    gtk_widget_set_no_show_all(app->error_bar, TRUE);
    gtk_widget_hide(app->error_bar);
    g_signal_connect(app->error_bar, "response", G_CALLBACK(on_error_bar_response), app);
    gtk_box_pack_start(GTK_BOX(main_area), app->error_bar, FALSE, FALSE, 0);
    app->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(app->stack), 100);
    gtk_box_pack_start(GTK_BOX(main_area), app->stack, TRUE, TRUE, 0);
    app->status_label = gtk_label_new("");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(app->status_label, 14);
    gtk_widget_set_margin_top(app->status_label, 8);
    gtk_widget_set_margin_bottom(app->status_label, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->status_label), "status-text");
    gtk_box_pack_start(GTK_BOX(status), app->status_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_area), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_area), status, FALSE, FALSE, 0);
    gtk_paned_pack2(GTK_PANED(body), main_area, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(body), 194);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), root);
    setup_views(app);
    gtk_widget_show_all(root);
    gtk_widget_hide(app->error_bar);
    gtk_widget_set_no_show_all(app->error_bar, TRUE);
}

static gchar *
asset_directory_for(const gchar *argv0)
{
    gchar *binary_dir = g_path_get_dirname(argv0);
    gchar *candidate = g_build_filename(binary_dir, "icons", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
        g_free(binary_dir);
        return candidate;
    }
    g_free(candidate);
    return g_build_filename(binary_dir, "assets", "icons", NULL);
}

static void
cleanup(EasyFile *app)
{
    g_ptr_array_unref(app->history);
    g_free(app->current_path);
    g_free(app->context_path);
    g_free(app->asset_dir);
    g_object_unref(app->filter);
    g_object_unref(app->store);
}

int
main(int argc, char **argv)
{
    EasyFile app = {0};
    gtk_init(&argc, &argv);
    app.asset_dir = asset_directory_for(argv[0]);
    app.history = g_ptr_array_new_with_free_func(g_free);
    app.history_index = -1;
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.window), "easy-file-window");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1120, 740);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_window_set_title(GTK_WINDOW(app.window), "Easy File");
    gchar *logo = g_build_filename(app.asset_dir, "..", "logo.png", NULL);
    if (g_file_test(logo, G_FILE_TEST_IS_REGULAR))
        gtk_window_set_icon_from_file(GTK_WINDOW(app.window), logo, NULL);
    g_free(logo);

    app.store = gtk_list_store_new(N_COLUMNS, GDK_TYPE_PIXBUF, GDK_TYPE_PIXBUF,
                                   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN,
                                   G_TYPE_STRING);
    app.filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(
        GTK_TREE_MODEL(app.store), NULL));
    build_ui(&app);
    gtk_tree_model_filter_set_visible_func(app.filter, filter_row_visible, &app, NULL);
    apply_css(app.window, argv[0]);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    const gchar *initial_path =
        argc > 1 && g_file_test(argv[1], G_FILE_TEST_IS_DIR)
            ? argv[1] : g_get_home_dir();
    load_directory(&app, initial_path, TRUE);
    gtk_widget_show(app.window);
    gtk_main();
    cleanup(&app);
    return 0;
}
