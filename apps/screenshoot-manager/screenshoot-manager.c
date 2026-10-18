#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#define APP_NAME "Screenshot Manager"
#define SCREENSHOT_PREFIX "Screenshot_"

typedef enum {
    CAPTURE_OK,
    CAPTURE_CANCELLED,
    CAPTURE_FAILED
} CaptureResult;

static GtkWidget *history_list;
static GtkWidget *main_window;
static gchar *program_dir;

static gchar *pictures_directory(void)
{
    const gchar *configured = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    const gchar *home = g_get_home_dir();
    gchar *upper = g_build_filename(home, "Pictures", NULL);
    gchar *lower = g_build_filename(home, "pictures", NULL);
    gchar *result = NULL;
    struct stat info;

    if (configured && g_stat(configured, &info) == 0 && S_ISDIR(info.st_mode)) {
        result = g_strdup(configured);
    } else if (g_stat(upper, &info) == 0 && S_ISDIR(info.st_mode)) {
        result = g_strdup(upper);
    } else if (g_stat(lower, &info) == 0 && S_ISDIR(info.st_mode)) {
        result = g_strdup(lower);
    } else {
        if (g_mkdir_with_parents(upper, 0755) == 0) {
            result = g_strdup(upper);
        }
    }

    g_free(upper);
    g_free(lower);
    return result;
}

static gchar *make_screenshot_path(const gchar *directory, GError **error)
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
    gchar *path = NULL;
    guint suffix;

    for (suffix = 0; suffix < 1000; suffix++) {
        gchar *name = suffix == 0
            ? g_strdup_printf("%s%s.png", SCREENSHOT_PREFIX, stamp)
            : g_strdup_printf("%s%s_%02u.png", SCREENSHOT_PREFIX, stamp, suffix);
        gchar *candidate = g_build_filename(directory, name, NULL);
        g_free(name);

        if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) {
            path = candidate;
            break;
        }
        g_free(candidate);
    }

    if (!path) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                    "Could not choose a unique screenshot filename.");
    }
    g_free(stamp);
    g_date_time_unref(now);
    return path;
}

static gboolean is_selection_cancel(const gchar *message)
{
    if (!message)
        return FALSE;

    gchar *lowercase = g_ascii_strdown(message, -1);
    gboolean cancelled = strstr(lowercase, "cancel") != NULL ||
                        strstr(lowercase, "no selection") != NULL ||
                        strstr(lowercase, "aborted") != NULL;
    g_free(lowercase);
    return cancelled;
}

static CaptureResult run_maim(gboolean select_region, GBytes **image,
                              gchar **error_message)
{
    gchar *maim = g_find_program_in_path("maim");
    if (!maim) {
        *error_message = g_strdup(
            "The screenshot tool 'maim' is not installed or is not in PATH.");
        return CAPTURE_FAILED;
    }

    gchar *arguments[] = {
        maim, "--quiet", "--hidecursor", "--format=png",
        select_region ? "--select" : NULL,
        NULL
    };
    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(
        (const gchar * const *)arguments,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &error);
    g_free(maim);
    if (!process) {
        *error_message = g_strdup_printf("Could not start maim: %s",
                                         error->message);
        g_error_free(error);
        return CAPTURE_FAILED;
    }

    GBytes *stdout_bytes = NULL;
    GBytes *stderr_bytes = NULL;
    gboolean communicated = g_subprocess_communicate(
        process, NULL, NULL, &stdout_bytes, &stderr_bytes, &error);
    gchar *stderr_text = stderr_bytes
        ? g_strndup(g_bytes_get_data(stderr_bytes, NULL),
                    g_bytes_get_size(stderr_bytes))
        : g_strdup("");
    CaptureResult result = CAPTURE_FAILED;

    if (!communicated) {
        *error_message = g_strdup_printf("Could not capture the screen: %s",
                                         error->message);
        g_error_free(error);
    } else if (!g_subprocess_get_successful(process)) {
        if (select_region && is_selection_cancel(stderr_text)) {
            result = CAPTURE_CANCELLED;
        } else {
            gint status = g_subprocess_get_if_exited(process)
                ? g_subprocess_get_exit_status(process) : -1;
            *error_message = *stderr_text
                ? g_strdup_printf("maim failed (exit status %d): %s",
                                  status, g_strstrip(stderr_text))
                : g_strdup_printf("maim failed with exit status %d.", status);
        }
    } else if (!stdout_bytes || g_bytes_get_size(stdout_bytes) == 0) {
        *error_message = g_strdup("maim completed without producing an image.");
    } else {
        *image = g_bytes_ref(stdout_bytes);
        result = CAPTURE_OK;
    }

    g_clear_pointer(&stdout_bytes, g_bytes_unref);
    g_clear_pointer(&stderr_bytes, g_bytes_unref);
    g_free(stderr_text);
    g_object_unref(process);
    return result;
}

static CaptureResult run_gnome_screenshot(gboolean select_region,
                                          const gchar *path,
                                          gchar **error_message)
{
    gchar *gnome_screenshot = g_find_program_in_path("gnome-screenshot");
    if (!gnome_screenshot) {
        *error_message = g_strdup(
            "Region capture requires slop for maim, or the fallback "
            "'gnome-screenshot' command. Neither is available.");
        return CAPTURE_FAILED;
    }

    gchar *file_argument = g_strdup_printf("--file=%s", path);
    const gchar *arguments[4] = { gnome_screenshot, NULL, file_argument, NULL };
    if (select_region)
        arguments[1] = "--area";
    else {
        arguments[1] = file_argument;
        arguments[2] = NULL;
    }
    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(
        arguments, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                   G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_free(file_argument);
    g_free(gnome_screenshot);
    if (!process) {
        *error_message = g_strdup_printf(
            "Could not start gnome-screenshot for region capture: %s",
            error->message);
        g_error_free(error);
        return CAPTURE_FAILED;
    }

    GBytes *stdout_bytes = NULL;
    GBytes *stderr_bytes = NULL;
    gboolean communicated = g_subprocess_communicate(
        process, NULL, NULL, &stdout_bytes, &stderr_bytes, &error);
    gchar *stderr_text = stderr_bytes
        ? g_strndup(g_bytes_get_data(stderr_bytes, NULL),
                    g_bytes_get_size(stderr_bytes))
        : g_strdup("");
    CaptureResult result = CAPTURE_FAILED;

    if (!communicated) {
        *error_message = g_strdup_printf(
            "Could not capture a region with gnome-screenshot: %s",
            error->message);
        g_error_free(error);
    } else if (!g_subprocess_get_successful(process)) {
        gint status = g_subprocess_get_if_exited(process)
            ? g_subprocess_get_exit_status(process) : -1;
        *error_message = *stderr_text
            ? g_strdup_printf("gnome-screenshot failed (exit status %d): %s",
                              status, g_strstrip(stderr_text))
            : g_strdup_printf(
                "gnome-screenshot failed with exit status %d.", status);
    } else {
        struct stat info;
        if (g_stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size == 0) {
            g_unlink(path);
            result = CAPTURE_CANCELLED;
        } else {
            result = CAPTURE_OK;
        }
    }

    g_free(stderr_text);
    g_clear_pointer(&stdout_bytes, g_bytes_unref);
    g_clear_pointer(&stderr_bytes, g_bytes_unref);
    g_object_unref(process);
    return result;
}

static gboolean copy_image_to_clipboard(const gchar *path, gchar **error_message)
{
    gchar *xclip = g_find_program_in_path("xclip");
    if (!xclip) {
        *error_message = g_strdup(
            "Screenshot saved, but clipboard copy failed: "
            "'xclip' is not installed or is not in PATH.");
        return FALSE;
    }

    const gchar *arguments[] = {
        xclip, "-selection", "clipboard", "-t", "image/png", "-i", path, NULL
    };
    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(
        arguments, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                   G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    g_free(xclip);
    if (!process) {
        *error_message = g_strdup_printf(
            "Screenshot saved, but could not start xclip: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    gboolean success = g_subprocess_wait_check(process, NULL, &error);
    if (!success) {
        *error_message = g_strdup_printf(
            "Screenshot saved, but clipboard copy failed: %s",
            error ? error->message : "xclip exited unsuccessfully.");
        g_clear_error(&error);
    }

    g_object_unref(process);
    return success;
}

static CaptureResult take_screenshot(gboolean select_region, gchar **saved_path,
                                     gchar **error_message)
{
    gchar *xclip = g_find_program_in_path("xclip");
    if (!xclip) {
        *error_message = g_strdup(
            "The clipboard tool 'xclip' is not installed or is not in PATH.");
        return CAPTURE_FAILED;
    }
    g_free(xclip);

    gchar *pictures = pictures_directory();
    if (!pictures) {
        *error_message = g_strdup_printf(
            "Could not create or access the Pictures folder in %s.",
            g_get_home_dir());
        return CAPTURE_FAILED;
    }

    GError *error = NULL;
    gchar *path = make_screenshot_path(pictures, &error);
    g_free(pictures);
    if (!path) {
        *error_message = g_strdup(error->message);
        g_error_free(error);
        return CAPTURE_FAILED;
    }

    gchar *slop = select_region ? g_find_program_in_path("slop") : NULL;
    gboolean use_gnome_fallback = select_region && !slop;
    g_free(slop);

    GBytes *image = NULL;
    CaptureResult result;
    if (use_gnome_fallback) {
        result = run_gnome_screenshot(TRUE, path, error_message);
    } else {
        result = run_maim(select_region, &image, error_message);
    }
    if (result == CAPTURE_FAILED && !use_gnome_fallback) {
        gchar *maim_error = g_steal_pointer(error_message);
        result = run_gnome_screenshot(select_region, path, error_message);
        if (result == CAPTURE_FAILED && *error_message) {
            gchar *combined_error = g_strdup_printf(
                "%s\nFallback capture also failed: %s",
                maim_error ? maim_error : "maim failed.",
                *error_message);
            g_free(*error_message);
            *error_message = combined_error;
        }
        g_free(maim_error);
    }
    if (result != CAPTURE_OK) {
        g_unlink(path);
        g_free(path);
        g_clear_pointer(&image, g_bytes_unref);
        return result;
    }

    if (image) {
        gsize image_size = 0;
        gconstpointer image_data = g_bytes_get_data(image, &image_size);
        if (!g_file_set_contents(path, image_data, image_size, &error)) {
            *error_message = g_strdup_printf("Could not save screenshot: %s",
                                             error->message);
            g_error_free(error);
            g_bytes_unref(image);
            g_free(path);
            return CAPTURE_FAILED;
        }
        g_bytes_unref(image);
    }
    *saved_path = path;

    if (!copy_image_to_clipboard(path, error_message))
        return CAPTURE_FAILED;
    return CAPTURE_OK;
}

static gboolean is_screenshot_name(const gchar *name)
{
    gsize length = strlen(name);
    return g_str_has_prefix(name, SCREENSHOT_PREFIX) &&
           length > 4 && g_ascii_strcasecmp(name + length - 4, ".png") == 0;
}

static void open_screenshot(GtkListBox *box, GtkListBoxRow *row,
                            gpointer user_data)
{
    (void)box;
    (void)user_data;
    const gchar *path = g_object_get_data(G_OBJECT(row), "screenshot-path");
    if (!path)
        return;

    GError *error = NULL;
    gchar *uri = g_filename_to_uri(path, NULL, &error);
    if (!uri || !g_app_info_launch_default_for_uri(uri, NULL, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not open screenshot");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog), "%s",
            error ? error->message : "No application is available to open PNG images.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
    }
    g_free(uri);
}

static void refresh_history(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(history_list));
    for (GList *item = children; item; item = item->next)
        gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);

    gchar *directory = pictures_directory();
    GDir *dir = directory ? g_dir_open(directory, 0, NULL) : NULL;
    GList *names = NULL;
    const gchar *name;
    if (dir) {
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (is_screenshot_name(name))
                names = g_list_prepend(names, g_strdup(name));
        }
        g_dir_close(dir);
    }
    names = g_list_sort(names, (GCompareFunc)g_strcmp0);
    names = g_list_reverse(names);

    guint count = 0;
    for (GList *item = names; item && count < 30; item = item->next, count++) {
        gchar *path = g_build_filename(directory, (const gchar *)item->data, NULL);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
            path, 168, 96, TRUE, NULL);
        GtkWidget *image = pixbuf
            ? gtk_image_new_from_pixbuf(pixbuf)
            : gtk_image_new_from_icon_name("image-x-generic",
                                           GTK_ICON_SIZE_DIALOG);
        if (pixbuf)
            g_object_unref(pixbuf);
        GtkWidget *label = gtk_label_new(item->data);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(content), image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), label, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(row), content);
        g_object_set_data_full(G_OBJECT(row), "screenshot-path", path, g_free);
        gtk_list_box_insert(GTK_LIST_BOX(history_list), row, -1);
    }

    if (count == 0) {
        GtkWidget *empty = gtk_label_new(
            "Your screenshots will appear here.");
        gtk_widget_set_margin_top(empty, 36);
        gtk_widget_set_margin_bottom(empty, 36);
        gtk_widget_set_name(empty, "empty-history");
        gtk_list_box_insert(GTK_LIST_BOX(history_list), empty, -1);
    }
    g_list_free_full(names, g_free);
    g_free(directory);
    gtk_widget_show_all(history_list);
}

static void show_capture_result(gboolean select_region)
{
    gchar *path = NULL;
    gchar *error_message = NULL;
    CaptureResult result = take_screenshot(select_region, &path, &error_message);

    if (result == CAPTURE_CANCELLED)
        return;
    if (path)
        refresh_history();

    GtkWidget *dialog;
    if (result == CAPTURE_OK) {
        dialog = gtk_message_dialog_new(
            GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
            "Screenshot saved and copied to clipboard");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog), "%s", path);
    } else {
        dialog = gtk_message_dialog_new(
            GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "%s", error_message ? error_message : "Screenshot failed.");
        if (path)
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(dialog), "Saved file: %s", path);
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(path);
    g_free(error_message);
}

static void on_capture_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    show_capture_result(GPOINTER_TO_INT(user_data));
}

static void open_pictures_folder(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    gchar *directory = pictures_directory();
    GError *error = NULL;
    gchar *uri = directory ? g_filename_to_uri(directory, NULL, &error) : NULL;
    if (!uri || !g_app_info_launch_default_for_uri(uri, NULL, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not open the Pictures folder");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog), "%s",
            error ? error->message : "The Pictures folder is not accessible.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
    }
    g_free(uri);
    g_free(directory);
}

static void load_app_assets(void)
{
    gchar *css_path = g_build_filename(program_dir, "screenshoot-manager.css", NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (gtk_css_provider_load_from_path(provider, css_path, &error)) {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_clear_error(&error);
    g_object_unref(provider);
    g_free(css_path);

    gchar *logo_path = g_build_filename(program_dir, "logo.png", NULL);
    if (g_file_test(logo_path, G_FILE_TEST_EXISTS))
        gtk_window_set_icon_from_file(GTK_WINDOW(main_window), logo_path, NULL);
    g_free(logo_path);
}

static void show_window(void)
{
    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 620, 560);
    gtk_container_set_border_width(GTK_CONTAINER(main_window), 18);
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    load_app_assets();

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_add(GTK_CONTAINER(main_window), root);

    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title = gtk_label_new("Screenshots");
    gtk_widget_set_name(title, "title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *subtitle = gtk_label_new(
        "Capture your desktop, then find recent images below.");
    gtk_widget_set_name(subtitle, "subtitle");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(heading), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), heading, FALSE, FALSE, 0);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *capture = gtk_button_new_with_label("Capture Screen");
    GtkWidget *select = gtk_button_new_with_label("Select Region");
    GtkWidget *open_folder = gtk_button_new_with_label("Open Pictures");
    gtk_widget_set_name(capture, "primary-action");
    gtk_box_pack_start(GTK_BOX(actions), capture, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), select, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), open_folder, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    g_signal_connect(capture, "clicked", G_CALLBACK(on_capture_clicked),
                     GINT_TO_POINTER(FALSE));
    g_signal_connect(select, "clicked", G_CALLBACK(on_capture_clicked),
                     GINT_TO_POINTER(TRUE));
    g_signal_connect(open_folder, "clicked",
                     G_CALLBACK(open_pictures_folder), NULL);

    GtkWidget *history_heading = gtk_label_new("Recent screenshots");
    gtk_widget_set_name(history_heading, "section-title");
    gtk_widget_set_halign(history_heading, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), history_heading, FALSE, FALSE, 3);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    history_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(history_list),
                                    GTK_SELECTION_NONE);
    g_signal_connect(history_list, "row-activated",
                     G_CALLBACK(open_screenshot), NULL);
    gtk_container_add(GTK_CONTAINER(scroll), history_list);

    refresh_history();
    gtk_widget_show_all(main_window);
}

static void set_program_directory(const gchar *argv0)
{
    GError *error = NULL;
    gchar *executable = g_file_read_link("/proc/self/exe", &error);
    if (executable) {
        program_dir = g_path_get_dirname(executable);
        g_free(executable);
    } else {
        g_clear_error(&error);
        gchar *resolved = g_path_is_absolute(argv0)
            ? g_strdup(argv0) : g_find_program_in_path(argv0);
        program_dir = resolved
            ? g_path_get_dirname(resolved) : g_get_current_dir();
        g_free(resolved);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && g_strcmp0(argv[1], "--help") == 0) {
        g_print("Usage: %s [--capture|--select]\n"
                "  --capture  Capture the full screen, save a PNG, and copy it to the clipboard\n"
                "  --select   Select a screen region, save a PNG, and copy it to the clipboard\n"
                "  no option  Open the screenshot manager window\n",
                argv[0]);
        return 0;
    }
    if (argc > 2 ||
        (argc == 2 && g_strcmp0(argv[1], "--capture") != 0 &&
         g_strcmp0(argv[1], "--select") != 0)) {
        g_printerr("Usage: %s [--capture|--select]\n", argv[0]);
        return 2;
    }

    set_program_directory(argv[0]);
    if (argc == 2) {
        gchar *path = NULL;
        gchar *error_message = NULL;
        gboolean select_region = g_strcmp0(argv[1], "--select") == 0;
        CaptureResult result = take_screenshot(select_region, &path,
                                               &error_message);
        if (result == CAPTURE_OK) {
            g_print("%s\n", path);
        } else if (result == CAPTURE_FAILED) {
            const gchar *message = error_message ? error_message
                                                 : "Screenshot failed.";
            g_printerr("%s\n", message);
            if (gtk_init_check(&argc, &argv)) {
                GtkWidget *dialog = gtk_message_dialog_new(
                    NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_CLOSE, "Screenshot failed");
                gtk_message_dialog_format_secondary_text(
                    GTK_MESSAGE_DIALOG(dialog), "%s", message);
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
            }
        }
        g_free(path);
        g_free(error_message);
        g_free(program_dir);
        return result == CAPTURE_OK || result == CAPTURE_CANCELLED ? 0 : 1;
    }

    gtk_init(&argc, &argv);
    show_window();
    gtk_main();
    g_free(program_dir);
    return 0;
}
