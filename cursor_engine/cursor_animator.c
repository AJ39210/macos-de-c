#include "cursor_animator.h"

#include <stdio.h>

#define TOTAL_ANIM_FRAMES 15
#define CURSOR_ANIMATION_TICK_MS 90

typedef struct {
    GdkCursor *normal;
    GdkCursor *link;
    GdkCursor *alternate;
    GdkCursor *move;
    GdkCursor *diagonal_1;
    GdkCursor *diagonal_2;
    GdkCursor *horizontal_resize;
    GdkCursor *vertical_resize;
    GdkCursor *text;
    GdkCursor *bonus_text;
    GdkCursor *precision;
    GdkCursor *help;
    GdkCursor *handwriting;
    GdkCursor *unavailable;
    GdkCursor *busy_frames[TOTAL_ANIM_FRAMES];
    GdkCursor *working_frames[TOTAL_ANIM_FRAMES];
    GPtrArray *windows;
    GdkCursor *current_cursor;
    SystemCursorAnimation animation;
    gboolean animation_active;
    int current_frame;
    gint64 animation_end_time;
    guint animation_source;
} SystemCursors;

static SystemCursors *sys_cursors;
static char cursor_directory[512];

static GdkCursor *load_cursor(GdkDisplay *display, const char *name,
                              int hotspot_x, int hotspot_y) {
    char path[1024];
    GError *error = NULL;
    GdkPixbuf *pixbuf;
    GdkCursor *cursor;

    snprintf(path, sizeof(path), "%s/%s", cursor_directory, name);
    pixbuf = gdk_pixbuf_new_from_file(path, &error);
    if (!pixbuf) {
        g_warning("Could not load cursor %s: %s", path,
                  error ? error->message : "unknown image error");
        g_clear_error(&error);
        return NULL;
    }
    cursor = gdk_cursor_new_from_pixbuf(display, pixbuf, hotspot_x, hotspot_y);
    g_object_unref(pixbuf);
    return cursor;
}

static void apply_cursor_to_windows(GdkCursor *cursor) {
    if (!sys_cursors || !cursor) return;
    GdkScreen *screen = gdk_screen_get_default();
    GdkWindow *root = screen ? gdk_screen_get_root_window(screen) : NULL;
    if (root) gdk_window_set_cursor(root, cursor);

    for (guint i = 0; i < sys_cursors->windows->len; i++) {
        GtkWidget *widget = g_ptr_array_index(sys_cursors->windows, i);
        GdkWindow *window = gtk_widget_get_window(widget);
        if (window) gdk_window_set_cursor(window, cursor);
    }
}

static gboolean update_cursor_animation(gpointer user_data) {
    (void)user_data;
    if (!sys_cursors || !sys_cursors->animation_active) {
        return G_SOURCE_CONTINUE;
    }
    if (sys_cursors->animation_end_time &&
        g_get_monotonic_time() >= sys_cursors->animation_end_time) {
        cursor_system_reset();
        return G_SOURCE_CONTINUE;
    }

    GdkCursor **frames = sys_cursors->animation == SYSTEM_CURSOR_BUSY
        ? sys_cursors->busy_frames : sys_cursors->working_frames;
    sys_cursors->current_frame =
        (sys_cursors->current_frame + 1) % TOTAL_ANIM_FRAMES;
    GdkCursor *frame = frames[sys_cursors->current_frame];
    if (frame) {
        sys_cursors->current_cursor = frame;
        apply_cursor_to_windows(frame);
    }
    return G_SOURCE_CONTINUE;
}

static void load_cursor_pack(GdkDisplay *display) {
    sys_cursors->normal = load_cursor(display, "Normal.png", 0, 0);
    sys_cursors->link = load_cursor(display, "Link.png", 4, 0);
    sys_cursors->alternate = load_cursor(display, "Alternate.png", 0, 0);
    sys_cursors->move = load_cursor(display, "Bonus-Move.png", 16, 16);
    sys_cursors->diagonal_1 =
        load_cursor(display, "Diagonal-Resize-1.png", 16, 16);
    sys_cursors->diagonal_2 =
        load_cursor(display, "Diagonal-Resize-2.png", 16, 16);
    sys_cursors->horizontal_resize =
        load_cursor(display, "Horizontal-Resize.png", 16, 16);
    sys_cursors->vertical_resize =
        load_cursor(display, "Vertical Resize.png", 16, 16);
    sys_cursors->text = load_cursor(display, "Text Select.png", 8, 8);
    sys_cursors->bonus_text = load_cursor(display, "Bonus-Text.png", 8, 8);
    sys_cursors->precision =
        load_cursor(display, "Precision Select.png", 16, 16);
    sys_cursors->help = load_cursor(display, "Help.png", 0, 0);
    sys_cursors->handwriting =
        load_cursor(display, "Handwriting.png", 8, 8);
    sys_cursors->unavailable =
        load_cursor(display, "Unavailable.png", 16, 16);

    for (int i = 0; i < TOTAL_ANIM_FRAMES; i++) {
        char filename[64];
        snprintf(filename, sizeof(filename), "Busy/Busy_page_%02d.png", i + 1);
        sys_cursors->busy_frames[i] = load_cursor(display, filename, 0, 0);
        snprintf(filename, sizeof(filename),
                 "Working/Working_page_%02d.png", i + 1);
        sys_cursors->working_frames[i] = load_cursor(display, filename, 0, 0);
    }
}

static GdkCursor *get_cursor(SystemCursorType cursor) {
    if (!sys_cursors) return NULL;
    switch (cursor) {
    case SYSTEM_CURSOR_LINK: return sys_cursors->link;
    case SYSTEM_CURSOR_ALTERNATE: return sys_cursors->alternate;
    case SYSTEM_CURSOR_MOVE: return sys_cursors->move;
    case SYSTEM_CURSOR_DIAGONAL_RESIZE_1: return sys_cursors->diagonal_1;
    case SYSTEM_CURSOR_DIAGONAL_RESIZE_2: return sys_cursors->diagonal_2;
    case SYSTEM_CURSOR_HORIZONTAL_RESIZE: return sys_cursors->horizontal_resize;
    case SYSTEM_CURSOR_VERTICAL_RESIZE: return sys_cursors->vertical_resize;
    case SYSTEM_CURSOR_TEXT: return sys_cursors->text;
    case SYSTEM_CURSOR_BONUS_TEXT: return sys_cursors->bonus_text;
    case SYSTEM_CURSOR_PRECISION: return sys_cursors->precision;
    case SYSTEM_CURSOR_HELP: return sys_cursors->help;
    case SYSTEM_CURSOR_HANDWRITING: return sys_cursors->handwriting;
    case SYSTEM_CURSOR_UNAVAILABLE: return sys_cursors->unavailable;
    case SYSTEM_CURSOR_NORMAL: default: return sys_cursors->normal;
    }
}

void init_cursor_system(GtkWidget *window, const char *cursor_base_path) {
    cleanup_cursor_system();
    sys_cursors = g_new0(SystemCursors, 1);
    sys_cursors->windows = g_ptr_array_new_with_free_func(g_object_unref);
    g_strlcpy(cursor_directory, cursor_base_path, sizeof(cursor_directory));
    load_cursor_pack(gdk_display_get_default());
    if (window) cursor_system_track_widget(window);
    cursor_system_set_cursor(SYSTEM_CURSOR_NORMAL);
    sys_cursors->animation_source =
        g_timeout_add(CURSOR_ANIMATION_TICK_MS, update_cursor_animation, NULL);
}

void cursor_system_track_widget(GtkWidget *window) {
    if (!sys_cursors || !window || !GTK_IS_WIDGET(window)) return;
    for (guint i = 0; i < sys_cursors->windows->len; i++) {
        if (g_ptr_array_index(sys_cursors->windows, i) == window) return;
    }
    g_ptr_array_add(sys_cursors->windows, g_object_ref(window));
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    if (gdk_window && sys_cursors->current_cursor) {
        gdk_window_set_cursor(gdk_window, sys_cursors->current_cursor);
    }
}

void cursor_system_set_widget_cursor(GtkWidget *window, SystemCursorType cursor) {
    GdkCursor *gdk_cursor = get_cursor(cursor);
    if (!window || !gdk_cursor) return;
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    if (gdk_window) gdk_window_set_cursor(gdk_window, gdk_cursor);
}

void cursor_system_set_cursor(SystemCursorType cursor) {
    GdkCursor *gdk_cursor = get_cursor(cursor);
    if (!gdk_cursor) return;
    sys_cursors->animation_active = FALSE;
    sys_cursors->animation_end_time = 0;
    sys_cursors->current_cursor = gdk_cursor;
    apply_cursor_to_windows(gdk_cursor);
}

void cursor_system_start_animation(SystemCursorAnimation animation,
                                   guint duration_ms) {
    if (!sys_cursors) return;
    sys_cursors->animation = animation;
    sys_cursors->animation_active = TRUE;
    sys_cursors->current_frame = 0;
    sys_cursors->animation_end_time = duration_ms
        ? g_get_monotonic_time() + ((gint64)duration_ms * 1000) : 0;
    GdkCursor **frames = animation == SYSTEM_CURSOR_BUSY
        ? sys_cursors->busy_frames : sys_cursors->working_frames;
    if (frames[0]) {
        sys_cursors->current_cursor = frames[0];
        apply_cursor_to_windows(frames[0]);
    }
}

void cursor_system_reset(void) {
    if (!sys_cursors) return;
    cursor_system_set_cursor(SYSTEM_CURSOR_NORMAL);
}

void cleanup_cursor_system(void) {
    if (!sys_cursors) return;
    if (sys_cursors->animation_source) {
        g_source_remove(sys_cursors->animation_source);
    }
    GdkScreen *screen = gdk_screen_get_default();
    GdkWindow *root = screen ? gdk_screen_get_root_window(screen) : NULL;
    if (root) gdk_window_set_cursor(root, NULL);
    for (guint i = 0; i < sys_cursors->windows->len; i++) {
        GtkWidget *widget = g_ptr_array_index(sys_cursors->windows, i);
        GdkWindow *window = gtk_widget_get_window(widget);
        if (window) gdk_window_set_cursor(window, NULL);
    }

    g_clear_object(&sys_cursors->normal);
    g_clear_object(&sys_cursors->link);
    g_clear_object(&sys_cursors->alternate);
    g_clear_object(&sys_cursors->move);
    g_clear_object(&sys_cursors->diagonal_1);
    g_clear_object(&sys_cursors->diagonal_2);
    g_clear_object(&sys_cursors->horizontal_resize);
    g_clear_object(&sys_cursors->vertical_resize);
    g_clear_object(&sys_cursors->text);
    g_clear_object(&sys_cursors->bonus_text);
    g_clear_object(&sys_cursors->precision);
    g_clear_object(&sys_cursors->help);
    g_clear_object(&sys_cursors->handwriting);
    g_clear_object(&sys_cursors->unavailable);
    for (int i = 0; i < TOTAL_ANIM_FRAMES; i++) {
        g_clear_object(&sys_cursors->busy_frames[i]);
        g_clear_object(&sys_cursors->working_frames[i]);
    }
    g_ptr_array_unref(sys_cursors->windows);
    g_free(sys_cursors);
    sys_cursors = NULL;
}
