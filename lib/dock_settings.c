#include "dock_settings.h"

#include <glib/gstdio.h>

static char *settings_path(void) {
    return g_build_filename(g_get_user_config_dir(), "macos-de", "dock.ini",
                            NULL);
}

GKeyFile *macos_de_dock_settings_load(void) {
    GKeyFile *settings = g_key_file_new();
    char *path = settings_path();
    GError *error = NULL;
    if (!g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_warning("Could not read dock settings %s: %s", path,
                      error->message);
        }
        g_clear_error(&error);
    }
    g_free(path);
    return settings;
}

gboolean macos_de_dock_settings_save(GKeyFile *settings) {
    char *path = settings_path();
    char *directory = g_path_get_dirname(path);
    GError *error = NULL;
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_warning("Could not create dock settings directory %s.", directory);
        g_free(directory);
        g_free(path);
        return FALSE;
    }

    gsize length;
    char *contents = g_key_file_to_data(settings, &length, &error);
    if (!contents) {
        g_warning("Could not serialize dock settings: %s", error->message);
        g_clear_error(&error);
        g_free(directory);
        g_free(path);
        return FALSE;
    }
    gboolean saved = g_file_set_contents(path, contents, (gssize)length, &error);
    if (!saved) {
        g_warning("Could not save dock settings %s: %s", path,
                  error->message);
        g_clear_error(&error);
    }
    g_free(contents);
    g_free(directory);
    g_free(path);
    return saved;
}

gboolean macos_de_dock_settings_is_pinned(GKeyFile *settings,
                                           const char *app_id) {
    GError *error = NULL;
    gboolean pinned = g_key_file_get_boolean(settings, "pinned-apps", app_id,
                                              &error);
    if (error) {
        if (!g_error_matches(error, G_KEY_FILE_ERROR,
                             G_KEY_FILE_ERROR_KEY_NOT_FOUND) &&
            !g_error_matches(error, G_KEY_FILE_ERROR,
                             G_KEY_FILE_ERROR_GROUP_NOT_FOUND)) {
            g_warning("Could not read dock pin for %s: %s", app_id,
                      error->message);
        }
        g_clear_error(&error);
    }
    return pinned;
}

void macos_de_dock_settings_set_pinned(GKeyFile *settings,
                                       const char *app_id,
                                       gboolean pinned) {
    if (pinned) {
        g_key_file_set_boolean(settings, "pinned-apps", app_id, TRUE);
    } else {
        g_key_file_remove_key(settings, "pinned-apps", app_id, NULL);
    }
}

char *macos_de_dock_settings_get_icon(GKeyFile *settings,
                                      const char *app_id) {
    GError *error = NULL;
    char *icon = g_key_file_get_string(settings, "custom-icons", app_id,
                                       &error);
    if (error) {
        if (!g_error_matches(error, G_KEY_FILE_ERROR,
                             G_KEY_FILE_ERROR_KEY_NOT_FOUND) &&
            !g_error_matches(error, G_KEY_FILE_ERROR,
                             G_KEY_FILE_ERROR_GROUP_NOT_FOUND)) {
            g_warning("Could not read custom icon for %s: %s", app_id,
                      error->message);
        }
        g_clear_error(&error);
    }
    return icon;
}

void macos_de_dock_settings_set_icon(GKeyFile *settings,
                                     const char *app_id,
                                     const char *icon_path) {
    if (icon_path) {
        g_key_file_set_string(settings, "custom-icons", app_id, icon_path);
    } else {
        g_key_file_remove_key(settings, "custom-icons", app_id, NULL);
    }
}
