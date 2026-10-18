#ifndef MACOS_DE_DOCK_SETTINGS_H
#define MACOS_DE_DOCK_SETTINGS_H

#include <glib.h>

GKeyFile *macos_de_dock_settings_load(void);
gboolean macos_de_dock_settings_save(GKeyFile *settings);
gboolean macos_de_dock_settings_is_pinned(GKeyFile *settings,
                                           const char *app_id);
void macos_de_dock_settings_set_pinned(GKeyFile *settings,
                                       const char *app_id,
                                       gboolean pinned);
char *macos_de_dock_settings_get_icon(GKeyFile *settings,
                                      const char *app_id);
void macos_de_dock_settings_set_icon(GKeyFile *settings,
                                     const char *app_id,
                                     const char *icon_path);

#endif
