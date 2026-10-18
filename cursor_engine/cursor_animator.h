#ifndef CURSOR_ANIMATOR_H
#define CURSOR_ANIMATOR_H

#include <gtk/gtk.h>

typedef enum {
    SYSTEM_CURSOR_NORMAL,
    SYSTEM_CURSOR_LINK,
    SYSTEM_CURSOR_ALTERNATE,
    SYSTEM_CURSOR_MOVE,
    SYSTEM_CURSOR_DIAGONAL_RESIZE_1,
    SYSTEM_CURSOR_DIAGONAL_RESIZE_2,
    SYSTEM_CURSOR_HORIZONTAL_RESIZE,
    SYSTEM_CURSOR_VERTICAL_RESIZE,
    SYSTEM_CURSOR_TEXT,
    SYSTEM_CURSOR_BONUS_TEXT,
    SYSTEM_CURSOR_PRECISION,
    SYSTEM_CURSOR_HELP,
    SYSTEM_CURSOR_HANDWRITING,
    SYSTEM_CURSOR_UNAVAILABLE
} SystemCursorType;

typedef enum {
    SYSTEM_CURSOR_WORKING,
    SYSTEM_CURSOR_BUSY
} SystemCursorAnimation;

// Initialize the custom cursor pack system-wide
void init_cursor_system(GtkWidget *window, const char *cursor_base_path);
void cursor_system_track_widget(GtkWidget *window);
void cursor_system_set_widget_cursor(GtkWidget *window, SystemCursorType cursor);
void cursor_system_set_cursor(SystemCursorType cursor);
void cursor_system_start_animation(SystemCursorAnimation animation,
                                   guint duration_ms);
void cursor_system_reset(void);
void cleanup_cursor_system(void);

#endif