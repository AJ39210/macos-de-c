#define _GNU_SOURCE
#include <string.h>
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "../../lib/dock_settings.h"
#include "../../cursor_engine/cursor_animator.h"

#define APPS_PER_PAGE 10

typedef struct {
    GtkWidget *flow_box;
    GtkEntry *search_entry;
    GtkWidget *title_label;
    
    char current_path[512];
    char view_mode[32]; 
    char search_query[256];
    int current_page;
    int total_pages;
    gboolean list_mode;
    
    char history[50][512];
    int history_index;
    int history_count;
    
    GList *all_items; 
} FinderApp;

typedef struct {
    FinderApp *finder;
    char *app_id;
} FinderItemContext;

static void load_path(FinderApp *finder, const char *path, const char *mode, gboolean push_history);
static void refresh_view(FinderApp *finder);
static gboolean on_finder_item_button_press(GtkWidget *widget,
                                             GdkEventButton *event,
                                             gpointer user_data);
static void free_finder_item_context(gpointer data, GClosure *closure);

static gchar *
desktop_application_path(const gchar *app_id)
{
    gchar *desktop_id = g_str_has_suffix(app_id, ".desktop")
        ? g_strdup(app_id) : g_strconcat(app_id, ".desktop", NULL);
    gchar *result = NULL;
    const gchar *user_data_dir = g_get_user_data_dir();
    gchar *user_applications = g_build_filename(
        user_data_dir, "applications", desktop_id, NULL);
    if (g_file_test(user_applications, G_FILE_TEST_IS_REGULAR))
        result = g_strdup(user_applications);
    g_free(user_applications);

    const gchar *const *system_data_dirs = g_get_system_data_dirs();
    for (guint i = 0; result == NULL && system_data_dirs[i] != NULL; i++) {
        gchar *candidate = g_build_filename(
            system_data_dirs[i], "applications", desktop_id, NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
            result = candidate;
        else
            g_free(candidate);
    }

    if (result == NULL) {
        gchar *candidate = g_build_filename(
            user_data_dir, "flatpak", "exports", "share", "applications",
            desktop_id, NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
            result = candidate;
        else
            g_free(candidate);
    }

    const gchar *extra_directories[] = {
        "/var/lib/flatpak/exports/share/applications",
        "/var/lib/snapd/desktop/applications",
        "/run/current-system/sw/share/applications",
        "/home/linuxbrew/.linuxbrew/share/applications",
        NULL
    };
    for (guint i = 0; result == NULL && extra_directories[i] != NULL; i++) {
        gchar *candidate = g_build_filename(
            extra_directories[i], desktop_id, NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
            result = candidate;
        else
            g_free(candidate);
    }
    g_free(desktop_id);
    return result;
}

static GDesktopAppInfo *
find_desktop_application_info(const gchar *app_id)
{
    gchar *desktop_path = desktop_application_path(app_id);
    GDesktopAppInfo *app_info = desktop_path != NULL
        ? g_desktop_app_info_new_from_filename(desktop_path) : NULL;
    g_free(desktop_path);
    return app_info;
}

static GtkWidget* create_sidebar_heading(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_top(label, 14);
    gtk_widget_set_margin_bottom(label, 4);
    
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(0.8));
    gtk_label_set_attributes(GTK_LABEL(label), attrs);
    pango_attr_list_unref(attrs);
    
    return label;
}

static GtkWidget* create_sidebar_item(const char *icon_name, const char *label_text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    return box;
}

static void on_sidebar_row_selected(GtkListBox *box, GtkListBoxRow *row,
                                   gpointer user_data) {
    (void)box;
    FinderApp *finder = (FinderApp *)user_data;
    if (!row) return;

    GtkWidget *child_box = gtk_bin_get_child(GTK_BIN(row));
    if (!GTK_IS_CONTAINER(child_box)) return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(child_box));
    if (!children || !g_list_next(children)) {
        g_list_free(children);
        return;
    }

    GtkWidget *label_widget = GTK_WIDGET(g_list_nth_data(children, 1));
    const char *text = gtk_label_get_text(GTK_LABEL(label_widget));
    g_list_free(children);

    char target_path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/root";

    if (strcmp(text, "Applications") == 0) {
        load_path(finder, "/usr/share/applications", "apps", TRUE);
    } else if (strcmp(text, "Desktop") == 0) {
        snprintf(target_path, sizeof(target_path), "%s/Desktop", home);
        load_path(finder, target_path, "files", TRUE);
    } else if (strcmp(text, "Documents") == 0) {
        snprintf(target_path, sizeof(target_path), "%s/Documents", home);
        load_path(finder, target_path, "files", TRUE);
    } else if (strcmp(text, "Downloads") == 0) {
        snprintf(target_path, sizeof(target_path), "%s/Downloads", home);
        load_path(finder, target_path, "files", TRUE);
    } else if (strcmp(text, "Recents") == 0) {
        load_path(finder, home, "files", TRUE);
    }
}

// Universal Scanner covering ALL Linux distributions, Package Managers, Flatpaks, Snaps, NixOS, Alpine, and LFS
static void scan_current_source(FinderApp *finder) {
    g_list_free_full(finder->all_items, g_free);
    finder->all_items = NULL;

    if (strcmp(finder->view_mode, "apps") == 0) {
        const char *home = getenv("HOME");
        char user_apps[512] = "";
        char nix_profile[512] = "";
        if (home) {
            snprintf(user_apps, sizeof(user_apps), "%s/.local/share/applications", home);
            snprintf(nix_profile, sizeof(nix_profile), "%s/.nix-profile/share/applications", home);
        }

        // Comprehensive list of app paths across Debian, Arch, Fedora, Gentoo, NixOS, Alpine, Flatpak, Snap, and LFS
        const char *scan_paths[] = {
            "/usr/share/applications",
            "/usr/local/share/applications",
            user_apps,
            nix_profile,
            "/var/lib/flatpak/exports/share/applications",
            "/home/linuxbrew/.linuxbrew/share/applications", // Homebrew on Linux
            "/var/lib/snapd/desktop/applications",
            "/etc/profiles/per-user/", // NixOS user profiles
            "/run/current-system/sw/share/applications", // NixOS system apps
            "/usr/local/share/lfs/applications", // Custom LFS paths
            "/opt",
            NULL
        };

        for (int p = 0; scan_paths[p] != NULL; p++) {
            if (!scan_paths[p] || scan_paths[p][0] == '\0') continue;
            DIR *dir = opendir(scan_paths[p]);
            if (!dir) continue;

            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;

                char display_name[256];
                strncpy(display_name, entry->d_name, sizeof(display_name) - 1);

                if (g_str_has_suffix(display_name, ".desktop")) {
                    char *ext = strrchr(display_name, '.');
                    if (ext) *ext = '\0';
                }

                if (strlen(finder->search_query) > 0) {
                    if (!strcasestr(display_name, finder->search_query)) continue;
                }

                gboolean exists = FALSE;
                for (GList *l = finder->all_items; l != NULL; l = l->next) {
                    if (strcmp((char *)l->data, display_name) == 0) {
                        exists = TRUE;
                        break;
                    }
                }
                if (!exists) {
                    finder->all_items = g_list_append(finder->all_items, g_strdup(display_name));
                }
            }
            closedir(dir);
        }

        // Also scan raw system binary PATH directories (/bin, /usr/bin, /usr/local/bin, /sbin) for executables
        const char *bin_paths[] = { "/usr/bin", "/usr/local/bin", "/bin", "/usr/sbin", NULL };
        for (int b = 0; bin_paths[b] != NULL; b++) {
            DIR *dir = opendir(bin_paths[b]);
            if (!dir) continue;

            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                
                char exec_name[256];
                strncpy(exec_name, entry->d_name, sizeof(exec_name) - 1);

                if (strlen(finder->search_query) > 0) {
                    if (!strcasestr(exec_name, finder->search_query)) continue;
                }

                gboolean exists = FALSE;
                for (GList *l = finder->all_items; l != NULL; l = l->next) {
                    if (strcasecmp((char *)l->data, exec_name) == 0) {
                        exists = TRUE;
                        break;
                    }
                }
                if (!exists) {
                    finder->all_items = g_list_append(finder->all_items, g_strdup(exec_name));
                }
            }
            closedir(dir);
        }

    } else {
        DIR *dir = opendir(finder->current_path);
        if (!dir) return;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            if (strlen(finder->search_query) > 0) {
                if (!strcasestr(entry->d_name, finder->search_query)) continue;
            }

            finder->all_items = g_list_append(finder->all_items, g_strdup(entry->d_name));
        }
        closedir(dir);
    }

    int total_count = g_list_length(finder->all_items);
    finder->total_pages = (total_count + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
    if (finder->total_pages < 1) finder->total_pages = 1;
    if (finder->current_page >= finder->total_pages) finder->current_page = 0;
}

static void free_finder_item_context(gpointer data, GClosure *closure) {
    (void)closure;
    FinderItemContext *context = data;
    g_free(context->app_id);
    g_free(context);
}

static FinderItemContext *copy_finder_item_context(
    const FinderItemContext *context) {
    FinderItemContext *copy = g_new0(FinderItemContext, 1);
    copy->finder = context->finder;
    copy->app_id = g_strdup(context->app_id);
    return copy;
}

static gboolean fit_icon_chooser(gpointer user_data) {
    GtkWindow *window = GTK_WINDOW(user_data);
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
    GdkMonitor *monitor = gdk_display_get_monitor_at_window(
        display, gtk_widget_get_window(GTK_WIDGET(window)));
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    int width = MIN(760, geometry.width);
    int height = MIN(540, MAX(1, geometry.height - 60));
    gtk_window_resize(window, width, height);
    gtk_window_move(window, geometry.x + (geometry.width - width) / 2,
                    geometry.y + (geometry.height - height) / 2);
    return G_SOURCE_REMOVE;
}

static void on_change_app_icon(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    FinderItemContext *context = user_data;
    GtkWidget *parent = gtk_widget_get_toplevel(
        GTK_WIDGET(context->finder->flow_box));
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Choose Application Icon", GTK_WINDOW(parent),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 540);
    if (!gtk_file_chooser_set_current_folder(
            GTK_FILE_CHOOSER(dialog), g_get_home_dir())) {
        g_warning("Could not open the home folder in the icon chooser.");
    }
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Image files");
    gtk_file_filter_add_mime_type(filter, "image/*");
    gtk_file_filter_add_pattern(filter, "*.png");
    gtk_file_filter_add_pattern(filter, "*.jpg");
    gtk_file_filter_add_pattern(filter, "*.jpeg");
    gtk_file_filter_add_pattern(filter, "*.svg");
    gtk_file_filter_add_pattern(filter, "*.webp");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    gtk_widget_show(dialog);
    guint resize_source = g_timeout_add(100, fit_icon_chooser, dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    GSource *source = g_main_context_find_source_by_id(NULL, resize_source);
    if (source) g_source_destroy(source);
    if (response == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialog));
        GError *error = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(filename, &error);
        if (!pixbuf) {
            GtkWidget *message = gtk_message_dialog_new(
                GTK_WINDOW(parent), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE, "Could not use that image: %s",
                error ? error->message : "unsupported image format");
            gtk_dialog_run(GTK_DIALOG(message));
            gtk_widget_destroy(message);
            g_clear_error(&error);
        } else {
            g_object_unref(pixbuf);
            GKeyFile *settings = macos_de_dock_settings_load();
            macos_de_dock_settings_set_icon(settings, context->app_id,
                                             filename);
            if (macos_de_dock_settings_save(settings)) {
                refresh_view(context->finder);
            }
            g_key_file_unref(settings);
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_toggle_dock_pin(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    FinderItemContext *context = user_data;
    GKeyFile *settings = macos_de_dock_settings_load();
    gboolean pinned =
        macos_de_dock_settings_is_pinned(settings, context->app_id);
    macos_de_dock_settings_set_pinned(settings, context->app_id, !pinned);
    if (!pinned)
        g_key_file_remove_key(settings, "hidden-launchers",
                              context->app_id, NULL);
    if (macos_de_dock_settings_save(settings)) {
        refresh_view(context->finder);
    }
    g_key_file_unref(settings);
}

static gchar *desktop_exec_value(const gchar *command) {
    GString *quoted = g_string_new("\"");
    for (const gchar *cursor = command; *cursor; cursor++) {
        if (*cursor == '\\' || *cursor == '"')
            g_string_append_c(quoted, '\\');
        if (*cursor == '%')
            g_string_append_c(quoted, '%');
        g_string_append_c(quoted, *cursor);
    }
    g_string_append_c(quoted, '"');
    return g_string_free(quoted, FALSE);
}

static gboolean finder_app_is_on_desktop(const gchar *app_id) {
    gchar *name = g_str_has_suffix(app_id, ".desktop")
        ? g_strdup(app_id) : g_strconcat(app_id, ".desktop", NULL);
    gchar *path = g_build_filename(g_get_home_dir(), "Desktop", name, NULL);
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS) ||
                      g_file_test(path, G_FILE_TEST_IS_SYMLINK);
    g_free(path);
    g_free(name);
    return exists;
}

static void on_toggle_desktop_shortcut(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    FinderItemContext *context = user_data;
    gchar *desktop_directory = g_build_filename(g_get_home_dir(), "Desktop", NULL);
    if (g_mkdir_with_parents(desktop_directory, 0755) != 0) {
        g_warning("Could not create Desktop directory %s.", desktop_directory);
        g_free(desktop_directory);
        return;
    }

    gchar *desktop_name = g_str_has_suffix(context->app_id, ".desktop")
        ? g_strdup(context->app_id) : g_strconcat(context->app_id, ".desktop", NULL);
    gchar *destination = g_build_filename(desktop_directory, desktop_name, NULL);
    if (finder_app_is_on_desktop(context->app_id)) {
        GFile *shortcut = g_file_new_for_path(destination);
        GError *error = NULL;
        if (!g_file_trash(shortcut, NULL, &error)) {
            g_warning("Could not remove desktop shortcut %s: %s",
                      destination, error->message);
            g_clear_error(&error);
        }
        g_object_unref(shortcut);
    } else {
        gchar *source = desktop_application_path(context->app_id);
        GError *error = NULL;
        gboolean created = FALSE;
        if (source) {
            GFile *shortcut = g_file_new_for_path(destination);
            if (!g_file_make_symbolic_link(shortcut, source, NULL, &error)) {
                g_warning("Could not create desktop application shortcut: %s",
                          error->message);
                g_clear_error(&error);
            } else {
                created = TRUE;
            }
            g_object_unref(shortcut);
        } else {
            gchar *command = g_find_program_in_path(context->app_id);
            if (command) {
                gchar *exec = desktop_exec_value(command);
                gchar *contents = g_strdup_printf(
                    "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
                    "Terminal=false\n",
                    context->app_id, exec);
                created = g_file_set_contents(destination, contents, -1, &error);
                if (!created) {
                    g_warning("Could not write desktop application shortcut: %s",
                              error->message);
                    g_clear_error(&error);
                }
                g_free(contents);
                g_free(exec);
                g_free(command);
            } else {
                g_warning("Could not find an application entry or executable for %s.",
                          context->app_id);
            }
        }
        g_free(source);
    }
    g_free(destination);
    g_free(desktop_name);
    g_free(desktop_directory);
}

static gboolean on_finder_item_button_press(GtkWidget *widget,
                                             GdkEventButton *event,
                                             gpointer user_data) {
    if (event->button != GDK_BUTTON_SECONDARY ||
        event->type != GDK_BUTTON_PRESS) return FALSE;

    GtkFlowBoxChild *child = gtk_flow_box_get_child_at_pos(
        GTK_FLOW_BOX(widget), (gint)event->x, (gint)event->y);
    if (!child) return FALSE;
    GtkWidget *item = gtk_bin_get_child(GTK_BIN(child));
    const char *app_id = g_object_get_data(G_OBJECT(item), "finder-app-id");
    if (!app_id) return FALSE;
    FinderItemContext context = {
        .finder = user_data,
        .app_id = (char *)app_id
    };
    GKeyFile *settings = macos_de_dock_settings_load();
    gboolean pinned =
        macos_de_dock_settings_is_pinned(settings, context.app_id);
    g_key_file_unref(settings);

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *change_icon = gtk_menu_item_new_with_label("Choose Icon…");
    GtkWidget *dock_item = gtk_menu_item_new_with_label(
        pinned ? "Remove from Dock" : "Add to Dock");
    gboolean on_desktop = finder_app_is_on_desktop(context.app_id);
    GtkWidget *desktop_item = gtk_menu_item_new_with_label(
        on_desktop ? "Remove from Desktop" : "Add to Desktop");
    FinderItemContext *icon_context = copy_finder_item_context(&context);
    FinderItemContext *pin_context = copy_finder_item_context(&context);
    FinderItemContext *desktop_context = copy_finder_item_context(&context);
    g_signal_connect_data(change_icon, "activate",
        G_CALLBACK(on_change_app_icon), icon_context,
        free_finder_item_context, 0);
    g_signal_connect_data(dock_item, "activate",
        G_CALLBACK(on_toggle_dock_pin), pin_context,
        free_finder_item_context, 0);
    g_signal_connect_data(desktop_item, "activate",
        G_CALLBACK(on_toggle_desktop_shortcut), desktop_context,
        free_finder_item_context, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), change_icon);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), dock_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), desktop_item);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

static void refresh_view(FinderApp *finder) {
    scan_current_source(finder);

    GList *children = gtk_container_get_children(GTK_CONTAINER(finder->flow_box));
    for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);

    GKeyFile *dock_settings = g_str_equal(finder->view_mode, "apps")
        ? macos_de_dock_settings_load() : NULL;
    int start_idx = finder->current_page * APPS_PER_PAGE;
    int count = 0;

    GList *iter = g_list_nth(finder->all_items, start_idx);
    while (iter && count < APPS_PER_PAGE && start_idx + count < g_list_length(finder->all_items)) {
        char *name = (char *)iter->data;

        GtkWidget *item_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_size_request(item_box,
                                    finder->list_mode ? 300 : 110,
                                    finder->list_mode ? 48 : 110);
        if (finder->list_mode) {
            gtk_orientable_set_orientation(GTK_ORIENTABLE(item_box),
                                           GTK_ORIENTATION_HORIZONTAL);
            gtk_box_set_spacing(GTK_BOX(item_box), 12);
        }
        gtk_widget_set_margin_start(item_box, 8);
        gtk_widget_set_margin_end(item_box, 8);
        gtk_widget_set_margin_top(item_box, 8);
        gtk_widget_set_margin_bottom(item_box, 8);

        GtkWidget *icon_widget = NULL;
        if (dock_settings) {
            char *custom_icon =
                macos_de_dock_settings_get_icon(dock_settings, name);
            if (custom_icon) {
                GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
                    custom_icon, finder->list_mode ? 32 : 64,
                    finder->list_mode ? 32 : 64, TRUE, NULL);
                if (pixbuf) {
                    icon_widget = gtk_image_new_from_pixbuf(pixbuf);
                    g_object_unref(pixbuf);
                }
                g_free(custom_icon);
            }
        }
        if (strcmp(finder->view_mode, "apps") == 0) {
            if (!icon_widget) {
                GDesktopAppInfo *app_info =
                    find_desktop_application_info(name);
                GIcon *app_icon = app_info
                    ? g_app_info_get_icon(G_APP_INFO(app_info)) : NULL;
                if (app_icon != NULL) {
                    icon_widget = gtk_image_new_from_gicon(
                        app_icon, finder->list_mode ? GTK_ICON_SIZE_DND
                                                    : GTK_ICON_SIZE_DIALOG);
                }
                if (app_info != NULL)
                    g_object_unref(app_info);
            }
            if (!icon_widget) {
                GtkIconTheme *icon_theme = gtk_icon_theme_get_default();
                if (gtk_icon_theme_has_icon(icon_theme, name))
                    icon_widget = gtk_image_new_from_icon_name(
                        name, GTK_ICON_SIZE_DIALOG);
                else {
                    gchar *lower_name = g_ascii_strdown(name, -1);
                    if (gtk_icon_theme_has_icon(icon_theme, lower_name))
                        icon_widget = gtk_image_new_from_icon_name(
                            lower_name, GTK_ICON_SIZE_DIALOG);
                    g_free(lower_name);
                }
            }
            if (icon_widget == NULL)
                icon_widget = gtk_image_new_from_icon_name(
                    "application-x-executable", GTK_ICON_SIZE_DIALOG);
        } else {
            icon_widget = gtk_image_new_from_icon_name("folder", GTK_ICON_SIZE_DIALOG);
        }

        GtkWidget *label = gtk_label_new(name);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        if (finder->list_mode) gtk_widget_set_halign(label, GTK_ALIGN_START);

        gtk_box_pack_start(GTK_BOX(item_box), icon_widget, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(item_box), label, FALSE, FALSE, 0);

        gtk_flow_box_insert(GTK_FLOW_BOX(finder->flow_box), item_box, -1);
        if (dock_settings) {
            g_object_set_data_full(G_OBJECT(item_box), "finder-app-id",
                                   g_strdup(name), g_free);
        }

        iter = g_list_next(iter);
        count++;
    }

    gtk_widget_show_all(finder->flow_box);
    if (dock_settings) g_key_file_unref(dock_settings);
}

static void load_path(FinderApp *finder, const char *path, const char *mode, gboolean push_history) {
    g_strlcpy(finder->current_path, path, sizeof(finder->current_path));
    g_strlcpy(finder->view_mode, mode, sizeof(finder->view_mode));
    finder->current_page = 0;
    finder->search_query[0] = '\0';
    gtk_entry_set_text(finder->search_entry, "");

    const char *title = "Finder";
    if (strcmp(mode, "apps") == 0) title = "Applications";
    else if (strstr(path, "Desktop")) title = "Desktop";
    else if (strstr(path, "Documents")) title = "Documents";
    else if (strstr(path, "Downloads")) title = "Downloads";
    else title = "Recents";

    gtk_label_set_text(GTK_LABEL(finder->title_label), title);

    if (push_history && finder->history_index < 49) {
        finder->history_index++;
        snprintf(finder->history[finder->history_index], sizeof(finder->history[0]), "%s|%s", path, mode);
        finder->history_count = finder->history_index + 1;
    }

    refresh_view(finder);
}

static void on_search_changed(GtkEntry *entry, gpointer user_data) {
    FinderApp *finder = (FinderApp *)user_data;
    const char *text = gtk_entry_get_text(entry);
    g_strlcpy(finder->search_query, text, sizeof(finder->search_query));
    finder->current_page = 0;
    refresh_view(finder);
}

static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
    FinderApp *finder = (FinderApp *)user_data;
    refresh_view(finder);
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
    FinderApp *finder = (FinderApp *)user_data;
    if (finder->history_index > 0) {
        finder->history_index--;
        char *hist = finder->history[finder->history_index];
        char p_copy[512], m_copy[32];
        sscanf(hist, "%511[^|]|%31s", p_copy, m_copy);
        load_path(finder, p_copy, m_copy, FALSE);
    } else if (finder->current_page > 0) {
        finder->current_page--;
        refresh_view(finder);
    }
}

static void on_forward_clicked(GtkButton *btn, gpointer user_data) {
    FinderApp *finder = (FinderApp *)user_data;
    if (finder->history_index < finder->history_count - 1) {
        finder->history_index++;
        char *hist = finder->history[finder->history_index];
        char p_copy[512], m_copy[32];
        sscanf(hist, "%511[^|]|%31s", p_copy, m_copy);
        load_path(finder, p_copy, m_copy, FALSE);
    } else if (finder->current_page < finder->total_pages - 1) {
        finder->current_page++;
        refresh_view(finder);
    }
}

static void on_grid_view_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    FinderApp *finder = (FinderApp *)user_data;
    finder->list_mode = FALSE;
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(finder->flow_box), 6);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(finder->flow_box), 1);
    refresh_view(finder);
}

static void on_list_view_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    FinderApp *finder = (FinderApp *)user_data;
    finder->list_mode = TRUE;
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(finder->flow_box), 1);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(finder->flow_box), 1);
    refresh_view(finder);
}

static void on_item_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data) {
    FinderApp *finder = (FinderApp *)user_data;
    GtkWidget *item_box = gtk_bin_get_child(GTK_BIN(child));
    GList *sub_children = gtk_container_get_children(GTK_CONTAINER(item_box));
    if (sub_children && g_list_next(sub_children)) {
        GtkWidget *label = GTK_WIDGET(g_list_nth_data(sub_children, 1));
        const char *name = gtk_label_get_text(GTK_LABEL(label));
        
        if (strcmp(finder->view_mode, "apps") == 0) {
            cursor_system_start_animation(SYSTEM_CURSOR_WORKING, 1800);
            GDesktopAppInfo *app = find_desktop_application_info(name);
            if (app) {
                GError *error = NULL;
                if (!g_app_info_launch(G_APP_INFO(app), NULL, NULL, &error)) {
                    g_warning("Failed to launch application %s: %s",
                              name, error->message);
                    g_clear_error(&error);
                }
                g_object_unref(app);
            } else {
                char *executable = g_find_program_in_path(name);
                if (!executable) {
                    g_warning("No desktop entry or executable was found for %s.", name);
                } else {
                    char *argv[] = {executable, NULL};
                    GError *error = NULL;
                    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                       NULL, NULL, NULL, &error)) {
                        g_warning("Failed to launch %s: %s",
                                  name, error->message);
                        g_clear_error(&error);
                    }
                    g_free(executable);
                }
            }
        } else {
            char *path = g_build_filename(finder->current_path, name, NULL);
            GFile *file = g_file_new_for_path(path);
            char *uri = g_file_get_uri(file);
            GError *error = NULL;
            if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
                g_warning("Failed to open %s: %s", path, error->message);
                g_clear_error(&error);
            }
            g_free(uri);
            g_object_unref(file);
            g_free(path);
        }
    }
    g_list_free(sub_children);
}

static char *find_cursor_pack(const char *program_path) {
    const char *installed =
        "/usr/share/macos-de/assets/system/cursor/default1";
    if (g_file_test(installed, G_FILE_TEST_IS_DIR)) return g_strdup(installed);
    const char *candidates[] = {
        "assets/system/cursor/default1",
        "build/assets/system/cursor/default1",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (g_file_test(candidates[i], G_FILE_TEST_IS_DIR)) {
            return g_strdup(candidates[i]);
        }
    }
    char *program_directory = g_path_get_dirname(program_path);
    char *relative = g_build_filename(program_directory, "../../../assets",
        "system/cursor/default1", NULL);
    g_free(program_directory);
    return relative;
}

static void load_finder_styles(GtkCssProvider *provider, const char *program_path) {
    GError *error = NULL;
    if (gtk_css_provider_load_from_path(provider, "finder.css", &error)) return;
    g_clear_error(&error);

    char *program_dir = g_path_get_dirname(program_path);
    char *css_path = g_build_filename(program_dir, "finder.css", NULL);
    if (!gtk_css_provider_load_from_path(provider, css_path, &error)) {
        g_warning("Failed to load Finder stylesheet %s: %s", css_path,
                  error ? error->message : "unknown stylesheet error");
        g_clear_error(&error);
    }
    g_free(css_path);
    g_free(program_dir);
}

static gboolean prefers_dark_theme(void) {
    char *settings_path = g_build_filename(
        g_get_user_config_dir(), "macos-de", "settings.ini", NULL);
    if (!g_file_test(settings_path, G_FILE_TEST_EXISTS)) {
        g_free(settings_path);
        return TRUE;
    }

    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(settings, settings_path,
                                   G_KEY_FILE_NONE, &error)) {
        g_warning("Could not load desktop appearance settings: %s",
                  error->message);
        g_clear_error(&error);
        g_key_file_unref(settings);
        g_free(settings_path);
        return TRUE;
    }

    char *theme = g_key_file_get_string(settings, "Appearance", "theme", NULL);
    gboolean dark = !theme || g_str_equal(theme, "dark");
    if (theme && !g_str_equal(theme, "dark") && !g_str_equal(theme, "light")) {
        g_warning("Ignoring unsupported desktop theme '%s'", theme);
        dark = TRUE;
    }
    g_free(theme);
    g_key_file_unref(settings);
    g_free(settings_path);
    return dark;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    FinderApp *finder = g_new0(FinderApp, 1);
    finder->current_page = 0;
    finder->history_index = -1;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Finder");
    gtk_window_set_default_size(GTK_WINDOW(window), 850, 520);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

    GtkCssProvider *provider = gtk_css_provider_new();
    load_finder_styles(provider, argv[0]);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    if (prefers_dark_theme()) {
        static const char dark_theme_css[] =
            "window { background-color: #202126; color: #f5f5f7; }"
            ".sidebar { background-color: #28292f; color: #ececf0;"
            " border-right-color: #42434b; }"
            ".sidebar label { color: #ececf0; }"
            ".toolbar { background-color: #292a30;"
            " border-bottom-color: #42434b; }"
            "flowbox { background-color: #202126; }"
            "flowboxchild:selected label { color: #ffffff; }"
            "searchbar, entry { background-color: #303137; color: #f5f5f7;"
            " border-color: #4a4b53; }";
        GtkCssProvider *theme_provider = gtk_css_provider_new();
        GError *error = NULL;
        if (!gtk_css_provider_load_from_data(
                theme_provider, dark_theme_css, -1, &error)) {
            g_warning("Could not apply Finder dark theme: %s", error->message);
            g_clear_error(&error);
        } else {
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(), GTK_STYLE_PROVIDER(theme_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        }
        g_object_unref(theme_provider);
    }

    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_hbox);

    // Sidebar
    GtkWidget *sidebar_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(sidebar_scrolled, 200, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_scrolled), "sidebar");
    gtk_box_pack_start(GTK_BOX(main_hbox), sidebar_scrolled, FALSE, FALSE, 0);

    GtkWidget *sidebar_list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(sidebar_scrolled), sidebar_list);

    const char *items[][2] = {
        {"document-open-recent", "Recents"},
        {"folder-documents", "Applications"},
        {"user-desktop", "Desktop"},
        {"folder", "Documents"},
        {"folder-download", "Downloads"}
    };

    for (int i = 0; i < 5; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), create_sidebar_item(items[i][0], items[i][1]));
        gtk_list_box_insert(GTK_LIST_BOX(sidebar_list), row, -1);
    }

    g_signal_connect(sidebar_list, "row-selected",
                     G_CALLBACK(on_sidebar_row_selected), finder);

    // Right Content Area
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(main_hbox), right_vbox, TRUE, TRUE, 0);

    // Toolbar
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "toolbar");
    gtk_box_pack_start(GTK_BOX(right_vbox), toolbar, FALSE, FALSE, 0);

    // Nav Buttons
    GtkWidget *nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(nav_box), "linked");
    GtkWidget *btn_back = gtk_button_new_from_icon_name("go-previous", GTK_ICON_SIZE_MENU);
    GtkWidget *btn_forward = gtk_button_new_from_icon_name("go-next", GTK_ICON_SIZE_MENU);
    gtk_container_add(GTK_CONTAINER(nav_box), btn_back);
    gtk_container_add(GTK_CONTAINER(nav_box), btn_forward);
    gtk_box_pack_start(GTK_BOX(toolbar), nav_box, FALSE, FALSE, 0);

    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), finder);
    g_signal_connect(btn_forward, "clicked", G_CALLBACK(on_forward_clicked), finder);

    // Refresh / Retry Button
    GtkWidget *btn_refresh = gtk_button_new_from_icon_name("view-refresh", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_refresh, FALSE, FALSE, 2);
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), finder);

    // Title Label
    GtkWidget *title_label = gtk_label_new("Applications");
    finder->title_label = title_label;
    PangoAttrList *title_attrs = pango_attr_list_new();
    pango_attr_list_insert(title_attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title_label), title_attrs);
    pango_attr_list_unref(title_attrs);
    gtk_box_pack_start(GTK_BOX(toolbar), title_label, FALSE, FALSE, 10);

    // View Toggles
    GtkWidget *view_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(view_box), "linked");
    GtkWidget *btn_grid = gtk_button_new_from_icon_name("view-grid-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *btn_list = gtk_button_new_from_icon_name("view-list-symbolic", GTK_ICON_SIZE_MENU);
    gtk_container_add(GTK_CONTAINER(view_box), btn_grid);
    gtk_container_add(GTK_CONTAINER(view_box), btn_list);
    gtk_box_pack_start(GTK_BOX(toolbar), view_box, FALSE, FALSE, 5);

    g_signal_connect(btn_grid, "clicked", G_CALLBACK(on_grid_view_clicked), finder);
    g_signal_connect(btn_list, "clicked", G_CALLBACK(on_list_view_clicked), finder);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), spacer, TRUE, TRUE, 0);

    // Search Entry
    GtkWidget *search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(search_entry, 220, -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Search");
    gtk_box_pack_end(GTK_BOX(toolbar), search_entry, FALSE, FALSE, 0);
    finder->search_entry = GTK_ENTRY(search_entry);

    g_signal_connect(search_entry, "changed",
                     G_CALLBACK(on_search_changed), finder);

    // Grid View
    GtkWidget *scrolled_grid = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_grid), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(right_vbox), scrolled_grid, TRUE, TRUE, 0);

    GtkWidget *flow_box = gtk_flow_box_new();
    gtk_widget_set_valign(flow_box, GTK_ALIGN_START);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_box), 6);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow_box), 1);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_box), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(scrolled_grid), flow_box);
    finder->flow_box = flow_box;

    g_signal_connect(flow_box, "child-activated", G_CALLBACK(on_item_activated), finder);
    gtk_widget_add_events(flow_box, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(flow_box, "button-press-event",
                     G_CALLBACK(on_finder_item_button_press), finder);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_list_box_select_row(GTK_LIST_BOX(sidebar_list),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list), 1));
    load_path(finder, "/usr/share/applications", "apps", TRUE);

    gtk_widget_show_all(window);
    char *cursor_path = find_cursor_pack(argv[0]);
    init_cursor_system(window, cursor_path);
    g_free(cursor_path);
    gtk_main();

    cleanup_cursor_system();
    g_list_free_full(finder->all_items, g_free);
    g_free(finder);
    return 0;
}