#define _GNU_SOURCE
#include "lock_screen.h"
#include "system_status.h"

#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <gdk/gdkx.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCK_SCREEN_BLANK_AFTER_SECONDS 180
#define LOCK_SCREEN_DIM_PERCENT 25

typedef struct {
    GtkWidget *window;
    GtkWidget *background;
    gint monitor_index;
} MonitorShield;

struct MacosDeLockScreen {
    Display *display;
    XScreenSaverInfo *screen_saver_info;
    GtkWidget *window;
    GtkWidget *background;
    GPtrArray *monitor_shields;
    gint primary_monitor_index;
    GtkWidget *stack;
    GtkWidget *login_page;
    GtkWidget *clock_time;
    GtkWidget *date_label;
    GtkWidget *entry;
    GtkWidget *error_label;
    GtkWidget *topbar_window;
    GtkWidget *power_button;
    GtkWidget *topbar_clock;
    GtkWidget *active_app_label;
    GtkWidget *status_icons;
    gchar *settings_path;
    gchar *default_wallpaper;
    GdkPixbuf *background_pixbuf;
    GdkPixbuf *panel_pixbuf;
    const gchar *username;
    guint poll_source;
    guint clock_source;
    guint timeout_minutes;
    gint64 retry_after_us;
    unsigned long previous_idle_ms;
    gint64 last_activity_us;
    int previous_brightness;
    int dim_brightness;
    gboolean brightness_saved;
    gboolean brightness_dimmed;
    gboolean locked;
    gboolean blanked;
    gboolean keyboard_grabbed;
};

typedef struct {
    const char *username;
    const char *password;
} PamConversationData;

static void load_timeout_setting(MacosDeLockScreen *lock_screen);
static gboolean user_disables_automatic_lock(const gchar *username);
static void raise_topbar(MacosDeLockScreen *lock_screen);
static GdkWindow *prepare_lock_window(GtkWidget *widget,
                                      const GdkRectangle *geometry);

static void
free_pam_answers(struct pam_response *answers, int count)
{
    if (answers == NULL)
        return;
    for (int i = 0; i < count; i++) {
        if (answers[i].resp != NULL) {
            volatile char *secret = (volatile char *)answers[i].resp;
            gsize length = strlen(answers[i].resp);
            for (gsize j = 0; j < length; j++)
                secret[j] = 0;
            free(answers[i].resp);
        }
    }
    free(answers);
}

static gboolean
pam_conversation(int message_count, const struct pam_message **messages,
                 struct pam_response **responses, void *user_data)
{
    PamConversationData *data = user_data;
    struct pam_response *answers = calloc((size_t)message_count,
                                          sizeof(*answers));
    if (answers == NULL)
        return PAM_BUF_ERR;

    for (int i = 0; i < message_count; i++) {
        switch (messages[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
            answers[i].resp = strdup(data->password);
            break;
        case PAM_PROMPT_ECHO_ON:
            answers[i].resp = strdup(data->username);
            break;
        case PAM_TEXT_INFO:
        case PAM_ERROR_MSG:
            break;
        default:
            free_pam_answers(answers, i);
            return PAM_CONV_ERR;
        }
        if ((messages[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
             messages[i]->msg_style == PAM_PROMPT_ECHO_ON) &&
             answers[i].resp == NULL) {
             free_pam_answers(answers, i + 1);
             return PAM_BUF_ERR;
        }
    }
    *responses = answers;
    return PAM_SUCCESS;
}

static gboolean
authenticate_user(const char *username, const char *password)
{
    PamConversationData data = { username, password };
    struct pam_conv conversation = { pam_conversation, &data };
    pam_handle_t *handle = NULL;
    int result = pam_start("macos-de", username, &conversation, &handle);
    if (result == PAM_SUCCESS)
        result = pam_authenticate(handle, PAM_SILENT);
    if (result == PAM_SUCCESS)
        result = pam_acct_mgmt(handle, PAM_SILENT);
    if (handle != NULL)
        pam_end(handle, result);
    return result == PAM_SUCCESS;
}

static gchar *
get_lock_wallpaper(MacosDeLockScreen *lock_screen)
{
    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    gchar *wallpaper = NULL;
    if (g_key_file_load_from_file(settings, lock_screen->settings_path,
                                  G_KEY_FILE_NONE, &error)) {
        wallpaper = g_key_file_get_string(
            settings, "Appearance", "lock_wallpaper", NULL);
        if (wallpaper == NULL || *wallpaper == '\0') {
            g_free(wallpaper);
            wallpaper = g_key_file_get_string(
                settings, "Appearance", "wallpaper", NULL);
        }
    } else if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_warning("Could not read lock-screen wallpaper setting: %s",
                  error->message);
    }
    g_clear_error(&error);
    g_key_file_unref(settings);
    if (wallpaper != NULL && g_file_test(wallpaper, G_FILE_TEST_IS_REGULAR))
        return wallpaper;
    if (wallpaper != NULL)
        g_warning("Configured lock-screen wallpaper is unavailable: %s",
                  wallpaper);
    g_free(wallpaper);

    GKeyFile *desktop_settings = g_key_file_new();
    if (g_key_file_load_from_file(desktop_settings, lock_screen->settings_path,
                                  G_KEY_FILE_NONE, NULL)) {
        wallpaper = g_key_file_get_string(
            desktop_settings, "Appearance", "wallpaper", NULL);
    }
    g_key_file_unref(desktop_settings);
    if (wallpaper != NULL && g_file_test(wallpaper, G_FILE_TEST_IS_REGULAR))
        return wallpaper;
    g_free(wallpaper);
    return g_strdup(lock_screen->default_wallpaper);
}

static GdkPixbuf *
load_wallpaper(const char *path, gint width, gint height)
{
    GError *error = NULL;
    GdkPixbuf *wallpaper = gdk_pixbuf_new_from_file_at_scale(
        path, width, height, TRUE, &error);
    if (wallpaper == NULL) {
        g_warning("Could not load lock-screen wallpaper %s: %s", path,
                  error ? error->message : "unknown image error");
        g_clear_error(&error);
    }
    return wallpaper;
}

static GdkPixbuf *
load_blurred_wallpaper(const char *path, gint width, gint height)
{
    GError *error = NULL;
    GdkPixbuf *source = gdk_pixbuf_new_from_file_at_scale(
        path, width, height, TRUE, &error);
    if (source == NULL) {
        g_warning("Could not load lock-screen wallpaper %s: %s", path,
                  error ? error->message : "unknown image error");
        g_clear_error(&error);
        return NULL;
    }
    gint blur_width = MAX(1, gdk_pixbuf_get_width(source) / 18);
    gint blur_height = MAX(1, gdk_pixbuf_get_height(source) / 18);
    GdkPixbuf *small = gdk_pixbuf_scale_simple(
        source, blur_width, blur_height, GDK_INTERP_BILINEAR);
    GdkPixbuf *blurred = small != NULL
        ? gdk_pixbuf_scale_simple(small, width, height, GDK_INTERP_HYPER)
        : NULL;
    g_clear_object(&small);
    g_object_unref(source);
    return blurred;
}

static void
paint_cover(cairo_t *cr, GdkPixbuf *pixbuf, gint width, gint height)
{
    gint image_width = gdk_pixbuf_get_width(pixbuf);
    gint image_height = gdk_pixbuf_get_height(pixbuf);
    double scale = MAX((double)width / image_width,
                       (double)height / image_height);
    double drawn_width = image_width * scale;
    double drawn_height = image_height * scale;
    cairo_save(cr);
    cairo_translate(cr, (width - drawn_width) / 2.0,
                    (height - drawn_height) / 2.0);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void
append_rounded_rectangle(cairo_t *cr, double x, double y, double width,
                         double height, double radius)
{
    double right = x + width;
    double bottom = y + height;
    cairo_new_sub_path(cr);
    cairo_arc(cr, right - radius, y + radius, radius, -G_PI_2, 0);
    cairo_arc(cr, right - radius, bottom - radius, radius, 0, G_PI_2);
    cairo_arc(cr, x + radius, bottom - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3 * G_PI_2);
    cairo_close_path(cr);
}

static gboolean
draw_lock_background(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    MacosDeLockScreen *lock_screen = user_data;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    if (lock_screen->blanked || lock_screen->background_pixbuf == NULL) {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        return FALSE;
    }

    paint_cover(cr, lock_screen->background_pixbuf,
                allocation.width, allocation.height);
    cairo_set_source_rgba(cr, 0.015, 0.02, 0.04, 0.16);
    cairo_paint(cr);

    if (widget == lock_screen->background &&
        lock_screen->panel_pixbuf != NULL) {
        double panel_x = 40;
        double panel_y = 28;
        double panel_width = MIN(330.0, allocation.width * 0.34);
        double panel_height = MAX(1.0, allocation.height - panel_y);
        cairo_save(cr);
        append_rounded_rectangle(cr, panel_x, panel_y, panel_width,
                                 panel_height, 30);
        cairo_clip(cr);
        paint_cover(cr, lock_screen->panel_pixbuf,
                    allocation.width, allocation.height);
        cairo_set_source_rgba(cr, 0.035, 0.045, 0.07, 0.24);
        cairo_paint(cr);
        cairo_restore(cr);
        append_rounded_rectangle(cr, panel_x, panel_y, panel_width,
                                 panel_height, 30);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.3);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);
    }
    return FALSE;
}

static gboolean
update_lock_clock(gpointer user_data)
{
    MacosDeLockScreen *lock_screen = user_data;
    GDateTime *now = g_date_time_new_now_local();
    gchar *time_text = g_date_time_format(now, "%H:%M");
    gchar *date_text = g_date_time_format(now, "%A, %-d %B");
    gtk_label_set_text(GTK_LABEL(lock_screen->clock_time), time_text);
    gtk_label_set_text(GTK_LABEL(lock_screen->date_label), date_text);
    g_free(time_text);
    g_free(date_text);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static void
show_login_page(MacosDeLockScreen *lock_screen)
{
    gtk_widget_grab_focus(lock_screen->entry);
}

static void
restore_topbar(MacosDeLockScreen *lock_screen)
{
    gtk_widget_set_sensitive(lock_screen->topbar_window, TRUE);
    gtk_widget_show(lock_screen->topbar_clock);
    gtk_widget_show(lock_screen->active_app_label);
    gtk_widget_show(lock_screen->status_icons);
    gtk_widget_show(lock_screen->topbar_window);
}

static void
restore_brightness(MacosDeLockScreen *lock_screen)
{
    if (!lock_screen->brightness_saved)
        return;
    if (!system_status_set_brightness_percent(
            lock_screen->previous_brightness))
        g_warning("Could not restore the brightness from before the lock screen.");
    lock_screen->brightness_saved = FALSE;
    lock_screen->brightness_dimmed = FALSE;
}

static void
restore_brightness_on_activity(MacosDeLockScreen *lock_screen)
{
    if (!lock_screen->brightness_saved || !lock_screen->brightness_dimmed)
        return;
    if (!system_status_set_brightness_percent(lock_screen->previous_brightness)) {
        g_warning("Could not restore display brightness after lock-screen activity.");
        return;
    }
    lock_screen->brightness_dimmed = FALSE;
}

static void
wake_lock_screen(MacosDeLockScreen *lock_screen)
{
    if (!lock_screen->blanked)
        return;
    lock_screen->blanked = FALSE;
    restore_brightness_on_activity(lock_screen);
    gtk_widget_show(lock_screen->stack);
    raise_topbar(lock_screen);
    gtk_widget_queue_draw(lock_screen->background);
    for (guint i = 0; i < lock_screen->monitor_shields->len; i++) {
        MonitorShield *shield = g_ptr_array_index(
            lock_screen->monitor_shields, i);
        gtk_widget_queue_draw(shield->background);
    }
    show_login_page(lock_screen);
}

static gboolean
query_screen_idle(MacosDeLockScreen *lock_screen, unsigned long *idle_ms)
{
    if (!XScreenSaverQueryInfo(
            lock_screen->display, DefaultRootWindow(lock_screen->display),
            lock_screen->screen_saver_info))
        return FALSE;
    *idle_ms = lock_screen->screen_saver_info->idle;
    return TRUE;
}

static void
raise_topbar(MacosDeLockScreen *lock_screen)
{
    gtk_widget_show(lock_screen->topbar_window);
    GdkWindow *window = gtk_widget_get_window(lock_screen->topbar_window);
    if (window != NULL) {
        XRaiseWindow(lock_screen->display, GDK_WINDOW_XID(window));
        XFlush(lock_screen->display);
    }
}

static void
show_monitor_shields(MacosDeLockScreen *lock_screen)
{
    GdkScreen *screen = gtk_widget_get_screen(lock_screen->window);
    GdkDisplay *display = gdk_screen_get_display(screen);
    for (guint i = 0; i < lock_screen->monitor_shields->len; i++) {
        MonitorShield *shield = g_ptr_array_index(
            lock_screen->monitor_shields, i);
        GdkMonitor *monitor = gdk_display_get_monitor(
            display, shield->monitor_index);
        GdkRectangle geometry;
        if (monitor == NULL)
            continue;
        gdk_monitor_get_geometry(monitor, &geometry);
        GdkWindow *window = prepare_lock_window(shield->window, &geometry);
        if (window == NULL)
            continue;
        gtk_widget_show_all(shield->window);
        gdk_window_move_resize(window, geometry.x, geometry.y,
                               geometry.width, geometry.height);
        gdk_window_show(window);
        gdk_window_raise(window);
    }
    XFlush(lock_screen->display);
}

static void
hide_monitor_shields(MacosDeLockScreen *lock_screen)
{
    for (guint i = 0; i < lock_screen->monitor_shields->len; i++) {
        MonitorShield *shield = g_ptr_array_index(
            lock_screen->monitor_shields, i);
        gtk_widget_hide(shield->window);
    }
}

static gboolean
wait_for_lock_window(MacosDeLockScreen *lock_screen, GdkWindow *window)
{
    if (window == NULL)
        return FALSE;

    Window xid = GDK_WINDOW_XID(window);
    gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    do {
        XWindowAttributes attributes;
        XSync(lock_screen->display, False);
        if (XGetWindowAttributes(lock_screen->display, xid, &attributes) &&
            attributes.map_state == IsViewable)
            return TRUE;
        g_usleep(10000);
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}

static GdkWindow *
prepare_lock_window(GtkWidget *widget, const GdkRectangle *geometry)
{
    gtk_widget_realize(widget);
    GdkWindow *window = gtk_widget_get_window(widget);
    if (window == NULL)
        return NULL;

    gdk_window_set_override_redirect(window, TRUE);
    gdk_window_move_resize(window, geometry->x, geometry->y,
                           geometry->width, geometry->height);
    return window;
}

static gboolean
start_lock_screen(MacosDeLockScreen *lock_screen, unsigned long idle_ms,
                  gboolean automatic, GError **error)
{
    GdkDisplay *gdk_display = gtk_widget_get_display(lock_screen->window);
    GdkRectangle geometry = {
        0, 0,
        DisplayWidth(lock_screen->display, DefaultScreen(lock_screen->display)),
        DisplayHeight(lock_screen->display, DefaultScreen(lock_screen->display))
    };
    if (lock_screen->primary_monitor_index >= 0) {
        GdkMonitor *monitor = gdk_display_get_monitor(
            gdk_display, lock_screen->primary_monitor_index);
        if (monitor != NULL)
            gdk_monitor_get_geometry(monitor, &geometry);
    }
    GdkWindow *gdk_window = prepare_lock_window(
        lock_screen->window, &geometry);
    if (gdk_window == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Could not create the lock-screen display window.");
        return FALSE;
    }

    gchar *wallpaper = get_lock_wallpaper(lock_screen);
    GdkPixbuf *background = load_wallpaper(
        wallpaper, MAX(1, geometry.width), MAX(1, geometry.height));
    GdkPixbuf *panel_background = load_blurred_wallpaper(
        wallpaper, MAX(1, geometry.width), MAX(1, geometry.height));
    g_free(wallpaper);
    g_set_object(&lock_screen->background_pixbuf, background);
    g_set_object(&lock_screen->panel_pixbuf, panel_background);
    g_clear_object(&background);
    g_clear_object(&panel_background);

    if (automatic) {
        unsigned long current_idle_ms;
        if (!query_screen_idle(lock_screen, &current_idle_ms)) {
            g_warning("Could not verify user activity before automatic locking.");
            return FALSE;
        }
        if (current_idle_ms < lock_screen->timeout_minutes * 60UL * 1000UL)
            return FALSE;
        idle_ms = current_idle_ms;
    }

    lock_screen->previous_brightness =
        system_status_get_brightness_percent();
    lock_screen->brightness_saved = lock_screen->previous_brightness >= 0;
    lock_screen->dim_brightness = MIN(
        LOCK_SCREEN_DIM_PERCENT, lock_screen->previous_brightness);
    lock_screen->previous_idle_ms = idle_ms;
    lock_screen->last_activity_us = g_get_monotonic_time();
    lock_screen->blanked = FALSE;
    lock_screen->locked = TRUE;
    gtk_stack_set_visible_child(GTK_STACK(lock_screen->stack),
                                lock_screen->login_page);
    gtk_widget_hide(lock_screen->topbar_clock);
    gtk_widget_hide(lock_screen->active_app_label);
    gtk_widget_hide(lock_screen->status_icons);
    gtk_widget_show(lock_screen->topbar_window);
    gtk_widget_show_all(lock_screen->window);
    gdk_window_move_resize(gdk_window, geometry.x, geometry.y,
                           geometry.width, geometry.height);
    gdk_window_show(gdk_window);
    gdk_window_raise(gdk_window);
    show_monitor_shields(lock_screen);
    raise_topbar(lock_screen);

    int result = GrabNotViewable;
    if (wait_for_lock_window(lock_screen, gdk_window)) {
        XSetInputFocus(lock_screen->display, GDK_WINDOW_XID(gdk_window),
                       RevertToPointerRoot, CurrentTime);
        XSync(lock_screen->display, False);
        result = XGrabKeyboard(
            lock_screen->display, GDK_WINDOW_XID(gdk_window), True,
            GrabModeAsync, GrabModeAsync, CurrentTime);
    }
    if (result == GrabSuccess) {
        lock_screen->keyboard_grabbed = TRUE;
        show_login_page(lock_screen);
        if (lock_screen->brightness_saved &&
            !system_status_set_brightness_percent(lock_screen->dim_brightness)) {
            g_warning("Could not dim the display for the lock screen.");
        } else if (lock_screen->brightness_saved) {
            lock_screen->brightness_dimmed = TRUE;
        }
    } else {
        g_warning("Could not securely grab keyboard for the lock screen (X11 status %d).",
                  result);
        if (error != NULL) {
            const char *reason = result == AlreadyGrabbed
                ? "The keyboard is already grabbed by another application."
                : result == GrabFrozen
                    ? "The keyboard is frozen by another application."
                    : result == GrabInvalidTime
                        ? "The X server rejected the keyboard grab time."
                        : "The lock-screen window did not become visible.";
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not grab the keyboard for the lock screen: %s (X11 status %d).",
                        reason, result);
        }
        lock_screen->locked = FALSE;
        gtk_widget_hide(lock_screen->window);
        hide_monitor_shields(lock_screen);
        restore_topbar(lock_screen);
        restore_brightness(lock_screen);
        lock_screen->retry_after_us =
            g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
        return FALSE;
    }
    if (lock_screen->locked)
        raise_topbar(lock_screen);
    return lock_screen->locked;
}

static void
blank_lock_screen(MacosDeLockScreen *lock_screen)
{
    lock_screen->blanked = TRUE;
    if (lock_screen->brightness_saved &&
        !system_status_set_brightness_percent(0))
        g_warning("Could not turn off the physical display backlight for lock-screen blanking.");
    gtk_widget_hide(lock_screen->stack);
    gtk_widget_hide(lock_screen->topbar_window);
    gtk_widget_queue_draw(lock_screen->background);
    for (guint i = 0; i < lock_screen->monitor_shields->len; i++) {
        MonitorShield *shield = g_ptr_array_index(
            lock_screen->monitor_shields, i);
        gtk_widget_queue_draw(shield->background);
    }
}

static gboolean
poll_lock_screen(gpointer user_data)
{
    MacosDeLockScreen *lock_screen = user_data;
    unsigned long idle_ms;
    if (!query_screen_idle(lock_screen, &idle_ms)) {
        g_warning("Could not query X11 screen idle time.");
        return G_SOURCE_CONTINUE;
    }

    if (lock_screen->locked) {
        if (idle_ms < lock_screen->previous_idle_ms) {
            lock_screen->last_activity_us = g_get_monotonic_time();
            if (lock_screen->blanked)
                wake_lock_screen(lock_screen);
            else
                restore_brightness_on_activity(lock_screen);
        }
        lock_screen->previous_idle_ms = idle_ms;
        gint64 inactive_us = g_get_monotonic_time() -
                             lock_screen->last_activity_us;
        if (!lock_screen->blanked &&
            inactive_us >= LOCK_SCREEN_BLANK_AFTER_SECONDS * G_USEC_PER_SEC)
            blank_lock_screen(lock_screen);
        return G_SOURCE_CONTINUE;
    }

    load_timeout_setting(lock_screen);
    if (lock_screen->timeout_minutes > 0 &&
        g_get_monotonic_time() >= lock_screen->retry_after_us &&
        idle_ms >= lock_screen->timeout_minutes * 60UL * 1000UL)
        start_lock_screen(lock_screen, idle_ms, TRUE, NULL);
    return G_SOURCE_CONTINUE;
}

gboolean
macos_de_lock_screen_lock_now(MacosDeLockScreen *lock_screen, GError **error)
{
    if (lock_screen == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "The desktop lock screen is unavailable.");
        return FALSE;
    }
    if (lock_screen->locked)
        return TRUE;

    unsigned long idle_ms;
    if (!query_screen_idle(lock_screen, &idle_ms)) {
        g_warning("Could not query X11 screen idle time for immediate locking.");
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Could not read X11 screen idle time.");
        return FALSE;
    }
    if (!start_lock_screen(lock_screen, idle_ms, FALSE, error))
        return FALSE;
    show_login_page(lock_screen);
    return TRUE;
}

static gboolean
on_lock_window_key_press(GtkWidget *widget, GdkEventKey *event,
                         gpointer user_data)
{
    MacosDeLockScreen *lock_screen = user_data;
    (void)widget;
    lock_screen->last_activity_us = g_get_monotonic_time();
    if (lock_screen->blanked)
        wake_lock_screen(lock_screen);
    else
        restore_brightness_on_activity(lock_screen);
    if (event->keyval == GDK_KEY_Escape)
        return TRUE;
    return FALSE;
}

static gboolean
on_lock_window_pointer_activity(GtkWidget *widget, GdkEvent *event,
                                gpointer user_data)
{
    MacosDeLockScreen *lock_screen = user_data;
    (void)widget;
    lock_screen->last_activity_us = g_get_monotonic_time();
    if (lock_screen->blanked)
        wake_lock_screen(lock_screen);
    else
        restore_brightness_on_activity(lock_screen);
    if (widget == lock_screen->window && event->type == GDK_BUTTON_PRESS &&
        lock_screen->power_button != NULL) {
        GtkAllocation allocation;
        gint button_x = 0;
        gint button_y = 0;
        gint topbar_x = 0;
        gint topbar_y = 0;
        GdkWindow *topbar_gdk_window =
            gtk_widget_get_window(lock_screen->topbar_window);
        gtk_widget_get_allocation(lock_screen->power_button, &allocation);
        if (topbar_gdk_window != NULL &&
            gtk_widget_translate_coordinates(
                lock_screen->power_button, lock_screen->topbar_window,
                0, 0, &button_x, &button_y)) {
            gdk_window_get_origin(topbar_gdk_window, &topbar_x, &topbar_y);
            GdkEventButton *button_event = (GdkEventButton *)event;
            if (button_event->x_root >= topbar_x + button_x &&
                button_event->x_root < topbar_x + button_x + allocation.width &&
                button_event->y_root >= topbar_y + button_y &&
                button_event->y_root < topbar_y + button_y + allocation.height)
                system_status_toggle_control_panel();
        }
    }
    return FALSE;
}

static gboolean
on_password_key_press(GtkWidget *widget, GdkEventKey *event,
                      gpointer user_data)
{
    MacosDeLockScreen *lock_screen = user_data;
    (void)widget;
    lock_screen->last_activity_us = g_get_monotonic_time();
    restore_brightness_on_activity(lock_screen);
    if (event->keyval == GDK_KEY_Escape)
        return TRUE;
    return FALSE;
}

static void
on_login_clicked(GtkButton *button, gpointer user_data)
{
    MacosDeLockScreen *lock_screen = user_data;
    const gchar *entry_text = gtk_entry_get_text(GTK_ENTRY(lock_screen->entry));
    gchar *password = g_strdup(entry_text);
    (void)button;

    gboolean authenticated = authenticate_user(lock_screen->username, password);
    volatile gchar *secret = (volatile gchar *)password;
    gsize secret_length = strlen(password);
    for (gsize i = 0; i < secret_length; i++)
        secret[i] = 0;
    g_free(password);
    gtk_entry_set_text(GTK_ENTRY(lock_screen->entry), "");
    lock_screen->last_activity_us = g_get_monotonic_time();

    if (!authenticated) {
        gtk_label_set_text(GTK_LABEL(lock_screen->error_label),
                           "Password not accepted. Please try again.");
        gtk_widget_grab_focus(lock_screen->entry);
        return;
    }

    lock_screen->locked = FALSE;
    lock_screen->blanked = FALSE;
    if (lock_screen->keyboard_grabbed) {
        XUngrabKeyboard(lock_screen->display, CurrentTime);
        lock_screen->keyboard_grabbed = FALSE;
    }
    restore_brightness(lock_screen);
    gtk_widget_hide(lock_screen->window);
    hide_monitor_shields(lock_screen);
    restore_topbar(lock_screen);
    unsigned long idle_ms;
    if (query_screen_idle(lock_screen, &idle_ms))
        lock_screen->previous_idle_ms = idle_ms;
}

static void
append_clock(GtkWidget *container, GtkWidget **time_out,
             GtkWidget **date_out)
{
    GtkWidget *clock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(clock, GTK_ALIGN_FILL);
    *time_out = gtk_label_new("");
    *date_out = gtk_label_new("");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(*time_out), "lock-clock");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(*date_out), "lock-date");
    gtk_label_set_xalign(GTK_LABEL(*time_out), 0.5f);
    gtk_label_set_xalign(GTK_LABEL(*date_out), 0.5f);
    gtk_box_pack_start(GTK_BOX(clock), *time_out, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(clock), *date_out, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(container), clock, FALSE, FALSE, 0);
}

static GtkWidget *
create_lock_login_page(MacosDeLockScreen *lock_screen)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *top_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bottom_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *greeting = gtk_label_new("Welcome back");
    gchar *username_text = g_strdup_printf("Sign in as %s", lock_screen->username);
    GtkWidget *username = gtk_label_new(username_text);
    GtkWidget *login_button = gtk_button_new_with_label("Unlock");
    g_free(username_text);

    lock_screen->login_page = page;
    gtk_widget_set_size_request(panel, 330, -1);
    gtk_widget_set_hexpand(panel, FALSE);
    gtk_widget_set_vexpand(panel, TRUE);
    gtk_widget_set_halign(panel, GTK_ALIGN_START);
    gtk_widget_set_margin_start(panel, 40);
    gtk_widget_set_margin_top(panel, 28);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "lock-panel");
    gtk_style_context_add_class(gtk_widget_get_style_context(greeting),
                                "lock-greeting");
    gtk_style_context_add_class(gtk_widget_get_style_context(username),
                                "lock-username");
    gtk_widget_set_halign(greeting, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(username, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(greeting), 0.5f);
    gtk_label_set_xalign(GTK_LABEL(username), 0.5f);
    gtk_widget_set_margin_bottom(greeting, 2);
    lock_screen->entry = gtk_entry_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(lock_screen->entry), "lock-entry");
    gtk_entry_set_visibility(GTK_ENTRY(lock_screen->entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(lock_screen->entry), 0x2022);
    gtk_entry_set_placeholder_text(GTK_ENTRY(lock_screen->entry),
                                   "Linux account password");
    gtk_entry_set_activates_default(GTK_ENTRY(lock_screen->entry), TRUE);
    gtk_widget_set_hexpand(lock_screen->entry, TRUE);
    lock_screen->error_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(lock_screen->error_label), TRUE);
    gtk_widget_set_halign(lock_screen->error_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lock_screen->error_label), 0.5f);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(lock_screen->error_label), "lock-error");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(login_button), "lock-button");
    gtk_widget_set_can_default(login_button, TRUE);
    gtk_widget_set_hexpand(login_button, TRUE);
    gtk_window_set_default(GTK_WINDOW(lock_screen->window), login_button);
    gtk_box_pack_start(GTK_BOX(panel), top_spacer, TRUE, TRUE, 0);
    append_clock(panel, &lock_screen->clock_time,
                 &lock_screen->date_label);
    gtk_box_pack_start(GTK_BOX(panel), greeting, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), username, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), lock_screen->entry, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(panel), lock_screen->error_label,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), login_button, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(panel), bottom_spacer, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), panel, FALSE, TRUE, 0);
    GtkWidget *wallpaper_space = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(wallpaper_space, TRUE);
    gtk_box_pack_start(GTK_BOX(page), wallpaper_space, TRUE, TRUE, 0);
    gtk_widget_add_events(lock_screen->entry, GDK_KEY_PRESS_MASK);
    g_signal_connect(lock_screen->entry, "key-press-event",
                     G_CALLBACK(on_password_key_press), lock_screen);
    g_signal_connect(login_button, "clicked",
                     G_CALLBACK(on_login_clicked), lock_screen);
    return page;
}

static void
load_timeout_setting(MacosDeLockScreen *lock_screen)
{
    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    gint timeout = 3;
    if (g_key_file_load_from_file(settings, lock_screen->settings_path,
                                  G_KEY_FILE_NONE, &error)) {
        if (g_key_file_has_key(settings, "LockScreen", "idle_minutes", NULL))
            timeout = g_key_file_get_integer(
                settings, "LockScreen", "idle_minutes", &error);
    } else if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_warning("Could not load lock-screen settings: %s", error->message);
    }
    if (error != NULL) {
        g_warning("Invalid lock-screen timeout setting: %s", error->message);
        g_clear_error(&error);
        timeout = 3;
    }
    if (timeout != 0 && timeout != 3 && timeout != 15 && timeout != 30 &&
        timeout != 60 && timeout != 120) {
        g_warning("Unsupported lock-screen timeout %d; using 3 minutes.",
                  timeout);
        timeout = 3;
    }
    lock_screen->timeout_minutes =
        user_disables_automatic_lock(lock_screen->username)
            ? 0 : (guint)timeout;
    g_key_file_unref(settings);
}

static gboolean
user_disables_automatic_lock(const gchar *username)
{
    return username != NULL &&
        (g_ascii_strcasecmp(username, "livesession") == 0 ||
         g_ascii_strcasecmp(username, "default") == 0 ||
         g_ascii_strcasecmp(username, "system") == 0);
}

gboolean
macos_de_lock_screen_should_lock_on_startup(
    const MacosDeLockScreen *lock_screen)
{
    return lock_screen != NULL &&
        !user_disables_automatic_lock(lock_screen->username);
}

static void
apply_lock_screen_css(void)
{
    static const gchar css[] =
        ".lock-panel { background-color: transparent; border: none;"
        " padding: 22px; }"
        ".lock-clock { color: white; font-size: 58px; font-weight: 300; }"
        ".lock-date { color: white; font-size: 18px; font-weight: 500; }"
        ".lock-greeting { color: white; font-size: 24px; font-weight: 600; }"
        ".lock-username { color: rgba(255,255,255,0.78); font-size: 15px; }"
        ".lock-error { color: #ff8c88; }"
        ".lock-panel entry.lock-entry { color: white;"
        " background-color: rgba(255,255,255,0.14);"
        " background-image: none; border: 1px solid rgba(255,255,255,0.22);"
        " border-radius: 12px; padding: 12px; }"
        ".lock-panel button.lock-button { color: white;"
        " background-color: #3768c5;"
        " background-image: none;"
        " border: 0; border-radius: 12px; padding: 12px; font-weight: 600; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_data(provider, css, -1, &error)) {
        g_warning("Could not load lock-screen styling: %s", error->message);
        g_clear_error(&error);
    } else {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    }
    g_object_unref(provider);
}

MacosDeLockScreen *
macos_de_lock_screen_new(GtkWidget *topbar_window, GtkWidget *power_button,
                         GtkWidget *topbar_clock,
                         GtkWidget *active_app_label, GtkWidget *status_icons,
                         const char *default_wallpaper)
{
    MacosDeLockScreen *lock_screen = g_new0(MacosDeLockScreen, 1);
    lock_screen->display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    lock_screen->screen_saver_info = XScreenSaverAllocInfo();
    lock_screen->topbar_window = topbar_window;
    lock_screen->power_button = power_button;
    lock_screen->topbar_clock = topbar_clock;
    lock_screen->active_app_label = active_app_label;
    lock_screen->status_icons = status_icons;
    lock_screen->settings_path = g_build_filename(
        g_get_user_config_dir(), "macos-de", "settings.ini", NULL);
    lock_screen->default_wallpaper = g_strdup(default_wallpaper);
    lock_screen->monitor_shields = g_ptr_array_new_with_free_func(g_free);
    lock_screen->primary_monitor_index = -1;
    struct passwd *account = getpwuid(getuid());
    lock_screen->username = account != NULL ? g_strdup(account->pw_name) : NULL;
    if (lock_screen->username == NULL) {
        g_warning("Could not determine current user for lock-screen authentication.");
        macos_de_lock_screen_free(lock_screen);
        return NULL;
    }

    int event_base, error_base;
    if (lock_screen->screen_saver_info == NULL ||
        !XScreenSaverQueryExtension(lock_screen->display,
                                    &event_base, &error_base)) {
        g_warning("X11 ScreenSaver extension is unavailable; automatic locking is disabled.");
        macos_de_lock_screen_free(lock_screen);
        return NULL;
    }
    load_timeout_setting(lock_screen);
    apply_lock_screen_css();

    lock_screen->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(lock_screen->window), "macOS-DE Lock Screen");
    gtk_window_set_decorated(GTK_WINDOW(lock_screen->window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(lock_screen->window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(lock_screen->window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(lock_screen->window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(lock_screen->window),
                             GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_accept_focus(GTK_WINDOW(lock_screen->window), TRUE);
    gtk_widget_add_events(lock_screen->window,
                          GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK |
                          GDK_POINTER_MOTION_MASK);
    g_signal_connect(lock_screen->window, "key-press-event",
                     G_CALLBACK(on_lock_window_key_press), lock_screen);
    g_signal_connect(lock_screen->window, "button-press-event",
                     G_CALLBACK(on_lock_window_pointer_activity), lock_screen);
    g_signal_connect(lock_screen->window, "motion-notify-event",
                     G_CALLBACK(on_lock_window_pointer_activity), lock_screen);
    gtk_widget_add_events(topbar_window,
                          GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(topbar_window, "button-press-event",
                     G_CALLBACK(on_lock_window_pointer_activity), lock_screen);
    g_signal_connect(topbar_window, "motion-notify-event",
                     G_CALLBACK(on_lock_window_pointer_activity), lock_screen);

    GtkWidget *overlay = gtk_overlay_new();
    lock_screen->background = gtk_drawing_area_new();
    gtk_widget_set_hexpand(lock_screen->background, TRUE);
    gtk_widget_set_vexpand(lock_screen->background, TRUE);
    g_signal_connect(lock_screen->background, "draw",
                     G_CALLBACK(draw_lock_background), lock_screen);
    gtk_container_add(GTK_CONTAINER(overlay), lock_screen->background);
    lock_screen->stack = gtk_stack_new();
    lock_screen->login_page = create_lock_login_page(lock_screen);
    gtk_stack_add_named(GTK_STACK(lock_screen->stack),
                        lock_screen->login_page, "login");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), lock_screen->stack);
    gtk_widget_set_hexpand(lock_screen->stack, TRUE);
    gtk_widget_set_vexpand(lock_screen->stack, TRUE);
    gtk_container_add(GTK_CONTAINER(lock_screen->window), overlay);
    GdkDisplay *gdk_display = gtk_widget_get_display(lock_screen->window);
    gint monitor_count = gdk_display_get_n_monitors(gdk_display);
    GdkMonitor *primary_monitor = gdk_display_get_primary_monitor(gdk_display);
    if (primary_monitor == NULL && monitor_count > 0)
        primary_monitor = gdk_display_get_monitor(gdk_display, 0);
    if (primary_monitor != NULL) {
        for (gint i = 0; i < monitor_count; i++) {
            GdkMonitor *monitor = gdk_display_get_monitor(gdk_display, i);
            if (monitor == primary_monitor) {
                lock_screen->primary_monitor_index = i;
                continue;
            }
            MonitorShield *shield = g_new0(MonitorShield, 1);
            shield->monitor_index = i;
            shield->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
            gtk_window_set_decorated(GTK_WINDOW(shield->window), FALSE);
            gtk_window_set_skip_taskbar_hint(GTK_WINDOW(shield->window), TRUE);
            gtk_window_set_skip_pager_hint(GTK_WINDOW(shield->window), TRUE);
            gtk_window_set_keep_above(GTK_WINDOW(shield->window), TRUE);
            gtk_window_set_type_hint(GTK_WINDOW(shield->window),
                                     GDK_WINDOW_TYPE_HINT_DOCK);
            gtk_window_set_accept_focus(GTK_WINDOW(shield->window), FALSE);
            shield->background = gtk_drawing_area_new();
            gtk_widget_set_hexpand(shield->background, TRUE);
            gtk_widget_set_vexpand(shield->background, TRUE);
            g_signal_connect(shield->background, "draw",
                             G_CALLBACK(draw_lock_background), lock_screen);
            gtk_container_add(GTK_CONTAINER(shield->window),
                              shield->background);
            g_ptr_array_add(lock_screen->monitor_shields, shield);
        }
    }
    update_lock_clock(lock_screen);
    lock_screen->clock_source = g_timeout_add_seconds(
        1, update_lock_clock, lock_screen);

    unsigned long idle_ms;
    if (query_screen_idle(lock_screen, &idle_ms))
        lock_screen->previous_idle_ms = idle_ms;
    lock_screen->poll_source = g_timeout_add_seconds(
        1, poll_lock_screen, lock_screen);
    return lock_screen;
}

void
macos_de_lock_screen_free(MacosDeLockScreen *lock_screen)
{
    if (lock_screen == NULL)
        return;
    if (lock_screen->poll_source != 0)
        g_source_remove(lock_screen->poll_source);
    if (lock_screen->clock_source != 0)
        g_source_remove(lock_screen->clock_source);
    if (lock_screen->keyboard_grabbed)
        XUngrabKeyboard(lock_screen->display, CurrentTime);
    restore_brightness(lock_screen);
    if (lock_screen->window != NULL)
        gtk_widget_destroy(lock_screen->window);
    hide_monitor_shields(lock_screen);
    for (guint i = 0; i < lock_screen->monitor_shields->len; i++) {
        MonitorShield *shield = g_ptr_array_index(
            lock_screen->monitor_shields, i);
        gtk_widget_destroy(shield->window);
    }
    g_ptr_array_unref(lock_screen->monitor_shields);
    if (lock_screen->screen_saver_info != NULL)
        XFree(lock_screen->screen_saver_info);
    g_clear_object(&lock_screen->background_pixbuf);
    g_clear_object(&lock_screen->panel_pixbuf);
    g_free((gchar *)lock_screen->username);
    g_free(lock_screen->settings_path);
    g_free(lock_screen->default_wallpaper);
    g_free(lock_screen);
}
