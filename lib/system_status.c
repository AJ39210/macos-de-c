#define _POSIX_C_SOURCE 200809L
#include "system_status.h"

#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    GtkWidget *power_button;
    GtkWidget *wifi_button;
    GtkWidget *battery_button;
    GtkWidget *network_detail;
    GtkWidget *battery_detail;
    GtkWidget *control_panel;
    GtkWidget *volume_scale;
    GtkWidget *brightness_scale;
    GtkWidget *mute_button;
    GtkWidget *volume_osd;
    GtkWidget *volume_osd_icon;
    GtkWidget *brightness_osd;
    GtkWidget *brightness_osd_icon;
    char *asset_directory;
    char *volume_backend;
    char *brightness_backend;
    char *brightness_device;
    gulong volume_event;
    gulong brightness_event;
    guint osd_hide_source;
    guint brightness_osd_hide_source;
    gboolean setting_scale;
    gboolean volume_muted;
    gboolean dark_theme;
} SystemStatus;

static SystemStatus *status;
static void launch_control_command(const char *const *argv);
static void show_brightness_osd(void);

static void append_rounded_rectangle(cairo_t *context, double width,
                                     double height, double radius) {
    double right = MAX(0.0, width - radius);
    double bottom = MAX(0.0, height - radius);
    cairo_new_sub_path(context);
    cairo_arc(context, right, radius, radius, -G_PI_2, 0);
    cairo_arc(context, right, bottom, radius, 0, G_PI_2);
    cairo_arc(context, radius, bottom, radius, G_PI_2, G_PI);
    cairo_arc(context, radius, radius, radius, G_PI, 3 * G_PI_2);
    cairo_close_path(context);
}

static gboolean draw_transparent_window(GtkWidget *window, cairo_t *context,
                                        gpointer user_data) {
    (void)user_data;
    GtkAllocation allocation;
    gtk_widget_get_allocation(window, &allocation);
    if (window == status->control_panel) {
        if (status->dark_theme) {
            cairo_set_source_rgba(context, 0.15, 0.15, 0.18, 0.96);
        } else {
            cairo_set_source_rgba(context, 0.96, 0.96, 0.97, 0.96);
        }
        append_rounded_rectangle(context, allocation.width,
                                 allocation.height, 16);
        cairo_fill_preserve(context);
        if (status->dark_theme) {
            cairo_set_source_rgba(context, 1, 1, 1, 0.14);
        } else {
            cairo_set_source_rgba(context, 0.08, 0.08, 0.11, 0.14);
        }
        cairo_set_line_width(context, 1);
        cairo_stroke(context);
    }
    return FALSE;
}

static void prepare_transparent_window(GtkWidget *window) {
    GdkScreen *screen = gtk_widget_get_screen(window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) {
        gtk_widget_set_app_paintable(window, TRUE);
        gtk_widget_set_visual(window, visual);
        g_signal_connect(window, "draw",
                         G_CALLBACK(draw_transparent_window), NULL);
    }
}

static char *asset_path(const char *subdirectory, const char *filename) {
    return g_build_filename(status->asset_directory, subdirectory, filename,
                            NULL);
}

static void set_icon_from_path(GtkWidget *image, const char *path, int size) {
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
        path, size, size, TRUE, NULL);
    if (pixbuf) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        g_object_unref(pixbuf);
    }
}

static void set_asset_icon(GtkWidget *image, const char *subdirectory,
                           const char *filename, int size) {
    char *path = asset_path(subdirectory, filename);
    set_icon_from_path(image, path, size);
    g_free(path);
}

static char *find_program(const char *const *programs) {
    for (int i = 0; programs[i]; i++) {
        char *path = g_find_program_in_path(programs[i]);
        if (path) return path;
    }
    return NULL;
}

static gboolean backend_is(const char *path, const char *name) {
    if (!path) return FALSE;
    const char *basename = strrchr(path, G_DIR_SEPARATOR);
    return g_str_equal(basename ? basename + 1 : path, name);
}

static char *capture_program(const char *path, const char *argument1,
                             const char *argument2) {
    if (!path) return NULL;
    char *argv[] = { (char *)path, (char *)argument1,
                     (char *)argument2, NULL };
    char *output = NULL;
    GError *error = NULL;
    gint exit_status = 0;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                     &output, NULL, &exit_status, &error) ||
        !g_spawn_check_wait_status(exit_status, &error)) {
        g_clear_error(&error);
        g_free(output);
        return NULL;
    }
    return output;
}

static char *find_backlight_device(void) {
    GError *error = NULL;
    GDir *directory = g_dir_open("/sys/class/backlight", 0, &error);
    if (!directory) {
        g_clear_error(&error);
        return NULL;
    }
    const char *name;
    char *device = NULL;
    while ((name = g_dir_read_name(directory)) != NULL) {
        char *brightness_path = g_build_filename(
            "/sys/class/backlight", name, "brightness", NULL);
        char *maximum_path = g_build_filename(
            "/sys/class/backlight", name, "max_brightness", NULL);
        char *brightness = NULL, *maximum = NULL;
        if (g_file_get_contents(brightness_path, &brightness, NULL, NULL) &&
            g_file_get_contents(maximum_path, &maximum, NULL, NULL) &&
            g_ascii_strtoll(maximum, NULL, 10) > 0)
            device = g_strdup(name);
        g_free(brightness_path);
        g_free(maximum_path);
        g_free(brightness);
        g_free(maximum);
        if (device != NULL)
            break;
    }
    g_dir_close(directory);
    return device;
}

static char *backlight_attribute_path(const char *attribute) {
    if (!status->brightness_device) return NULL;
    return g_build_filename("/sys/class/backlight",
                            status->brightness_device, attribute, NULL);
}

static gboolean backlight_is_writable(void) {
    if (!status->brightness_device)
        return status->brightness_backend != NULL;
    char *path = backlight_attribute_path("brightness");
    gboolean writable = path != NULL && access(path, W_OK) == 0;
    g_free(path);
    return writable;
}

static int read_backlight_percent(void) {
    char *brightness_path = backlight_attribute_path("brightness");
    char *maximum_path = backlight_attribute_path("max_brightness");
    char *brightness_text = NULL, *maximum_text = NULL;
    int percent = -1;
    if (brightness_path && maximum_path &&
        g_file_get_contents(brightness_path, &brightness_text, NULL, NULL) &&
        g_file_get_contents(maximum_path, &maximum_text, NULL, NULL)) {
        char *brightness_end = NULL, *maximum_end = NULL;
        gint64 brightness = g_ascii_strtoll(brightness_text, &brightness_end, 10);
        gint64 maximum = g_ascii_strtoll(maximum_text, &maximum_end, 10);
        if (brightness_end != brightness_text && maximum_end != maximum_text &&
            maximum > 0)
            percent = (int)CLAMP((brightness * 100 + maximum / 2) / maximum,
                                 0, 100);
    }
    g_free(brightness_path);
    g_free(maximum_path);
    g_free(brightness_text);
    g_free(maximum_text);
    return percent;
}

static gboolean write_backlight_percent(double percent) {
    char *brightness_path = backlight_attribute_path("brightness");
    char *maximum_path = backlight_attribute_path("max_brightness");
    char *maximum_text = NULL;
    gboolean success = FALSE;
    if (!brightness_path || !maximum_path ||
        !g_file_get_contents(maximum_path, &maximum_text, NULL, NULL))
        goto done;

    char *end = NULL;
    gint64 maximum = g_ascii_strtoll(maximum_text, &end, 10);
    if (end == maximum_text || maximum <= 0) {
        g_warning("Invalid max_brightness value for %s",
                  status->brightness_device);
        goto done;
    }
    gint64 value = (gint64)((maximum * CLAMP(percent, 0.0, 100.0) + 50.0) /
                            100.0);
    char value_text[32];
    int value_length = g_snprintf(value_text, sizeof(value_text), "%" G_GINT64_FORMAT,
                                  value);
    int fd = open(brightness_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        g_warning("Cannot write display brightness at %s: %s",
                  brightness_path, g_strerror(errno));
        goto done;
    }
    ssize_t written;
    do {
        written = write(fd, value_text, (size_t)value_length);
    } while (written < 0 && errno == EINTR);
    if (written != value_length) {
        int write_error = written < 0 ? errno : EIO;
        g_warning("Could not update display brightness at %s: %s",
                  brightness_path, g_strerror(write_error));
    } else {
        success = TRUE;
    }
    close(fd);
done:
    g_free(brightness_path);
    g_free(maximum_path);
    g_free(maximum_text);
    return success;
}

static int parse_percent(const char *text) {
    if (!text) return -1;
    const char *percent = strchr(text, '%');
    if (!percent) {
        while (*text && !g_ascii_isdigit(*text)) text++;
        if (!*text) return -1;
        return CLAMP((int)g_ascii_strtod(text, NULL), 0, 100);
    }
    const char *start = percent;
    while (start > text && g_ascii_isdigit(start[-1])) start--;
    if (start == percent) return -1;
    return CLAMP((int)g_ascii_strtod(start, NULL), 0, 100);
}

static int read_volume_percent(gboolean *muted) {
    *muted = FALSE;
    char *output;
    if (backend_is(status->volume_backend, "wpctl")) {
        output = capture_program(status->volume_backend, "get-volume",
                                 "@DEFAULT_AUDIO_SINK@");
    } else if (backend_is(status->volume_backend, "pactl")) {
        output = capture_program(status->volume_backend, "get-sink-volume",
                                 "@DEFAULT_SINK@");
    } else {
        output = capture_program(status->volume_backend, "get", "Master");
    }
    if (!output) return -1;
    *muted = strstr(output, "MUTED") || strstr(output, "[off]") ||
             strstr(output, "Mute: yes");
    int value;
    if (backend_is(status->volume_backend, "wpctl")) {
        const char *volume = strstr(output, "Volume:");
        value = volume
            ? CLAMP((int)(g_ascii_strtod(volume + strlen("Volume:"), NULL) *
                          100.0 + 0.5), 0, 100)
            : -1;
    } else {
        value = parse_percent(output);
    }
    g_free(output);
    return value;
}

static int read_brightness_percent(void) {
    if (status->brightness_device)
        return read_backlight_percent();
    if (backend_is(status->brightness_backend, "xbacklight")) {
        char *output = capture_program(status->brightness_backend, "-get", NULL);
        if (output) {
            int value = CLAMP((int)g_ascii_strtod(output, NULL), 0, 100);
            g_free(output);
            return value;
        }
    }
    return -1;
}

int system_status_get_brightness_percent(void) {
    return status != NULL ? read_brightness_percent() : -1;
}

gboolean system_status_set_brightness_percent(int percent) {
    if (status == NULL)
        return FALSE;
    if (status->brightness_device != NULL)
        return write_backlight_percent(CLAMP(percent, 0, 100));
    if (backend_is(status->brightness_backend, "xbacklight")) {
        char value[16];
        g_snprintf(value, sizeof(value), "%d", CLAMP(percent, 0, 100));
        const char *argv[] = {
            status->brightness_backend, "-set", value, NULL
        };
        launch_control_command(argv);
        return TRUE;
    }
    return FALSE;
}

static void on_control_finished(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
    (void)user_data;
    GError *error = NULL;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        g_warning("System control command failed: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(source);
}

static void launch_control_command(const char *const *argv) {
    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error);
    if (!process) {
        g_warning("Could not start system control command %s: %s",
                  argv[0], error->message);
        g_clear_error(&error);
        return;
    }
    g_subprocess_wait_check_async(process, NULL, on_control_finished, NULL);
}

static void set_volume(double volume) {
    char percent[16];
    g_snprintf(percent, sizeof(percent), "%.0f%%", CLAMP(volume, 0, 100));
    if (backend_is(status->volume_backend, "wpctl")) {
        const char *argv[] = { status->volume_backend, "set-volume",
                               "@DEFAULT_AUDIO_SINK@", percent, NULL };
        launch_control_command(argv);
    } else if (backend_is(status->volume_backend, "pactl")) {
        const char *argv[] = { status->volume_backend, "set-sink-volume",
                               "@DEFAULT_SINK@", percent, NULL };
        launch_control_command(argv);
    } else if (backend_is(status->volume_backend, "amixer")) {
        const char *argv[] = { status->volume_backend, "-q", "sset", "Master",
                               percent, NULL };
        launch_control_command(argv);
    }
}

static void set_brightness(double brightness) {
    system_status_set_brightness_percent(
        (int)(CLAMP(brightness, 0, 100) + 0.5));
}

static void on_volume_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    if (status->setting_scale) return;
    double value = gtk_range_get_value(range);
    set_volume(value);
}

static void on_brightness_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    if (!status->setting_scale) set_brightness(gtk_range_get_value(range));
}

static void on_volume_mute_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (backend_is(status->volume_backend, "wpctl")) {
        const char *argv[] = { status->volume_backend, "set-mute",
                               "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
        launch_control_command(argv);
    } else if (backend_is(status->volume_backend, "pactl")) {
        const char *argv[] = { status->volume_backend, "set-sink-mute",
                               "@DEFAULT_SINK@", "toggle", NULL };
        launch_control_command(argv);
    } else if (backend_is(status->volume_backend, "amixer")) {
        const char *argv[] = { status->volume_backend, "-q", "sset", "Master",
                               "toggle", NULL };
        launch_control_command(argv);
    }
    status->volume_muted = !status->volume_muted;
    gtk_button_set_label(button, status->volume_muted ? "Unmute" : "Mute");
}

static GtkWidget *create_status_row(const char *icon, const char *label,
                                    GtkWidget **detail_out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    GtkWidget *text = gtk_label_new(label);
    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_widget_set_hexpand(text, TRUE);
    *detail_out = text;
    gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    return row;
}

static GtkWidget *create_power_button(const char *label, const char *icon,
                                      GCallback callback) {
    GtkWidget *button = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    GtkWidget *text = gtk_label_new(label);
    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(button), row);
    gtk_style_context_add_class(gtk_widget_get_style_context(button),
                                "control-power-action");
    g_signal_connect(button, "clicked", callback, NULL);
    return button;
}

static void show_control_panel(GtkWidget *anchor) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(anchor, &allocation);
    int anchor_x = 0;
    int anchor_y = 0;
    GdkWindow *anchor_window = gtk_widget_get_window(anchor);
    if (anchor_window) gdk_window_get_origin(anchor_window, &anchor_x, &anchor_y);
    anchor_x += allocation.x;
    anchor_y += allocation.y;

    GtkRequisition minimum;
    GtkRequisition natural;
    gtk_widget_get_preferred_size(status->control_panel, &minimum, &natural);
    (void)minimum;
    GdkDisplay *display = gtk_widget_get_display(anchor);
    GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, anchor_window);
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);
    GdkRectangle geometry = {0, 0, 1280, 800};
    if (monitor) gdk_monitor_get_geometry(monitor, &geometry);

    int x = CLAMP(anchor_x, geometry.x,
                  geometry.x + geometry.width - natural.width);
    int y = anchor_y + allocation.height + 6;
    if (y + natural.height > geometry.y + geometry.height) {
        y = geometry.y + geometry.height - natural.height - 8;
    }
    gtk_window_move(GTK_WINDOW(status->control_panel), x, y);
    gtk_window_resize(GTK_WINDOW(status->control_panel), natural.width,
                      natural.height);
    gtk_widget_show_all(status->control_panel);
    gtk_window_present(GTK_WINDOW(status->control_panel));
    GdkWindow *panel_window = gtk_widget_get_window(status->control_panel);
    if (panel_window != NULL)
        gdk_window_raise(panel_window);
}

static void on_status_button_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (gtk_widget_get_visible(status->control_panel)) {
        gtk_widget_hide(status->control_panel);
    } else {
        show_control_panel(GTK_WIDGET(button));
    }
}

void system_status_toggle_control_panel(void) {
    if (status != NULL && status->power_button != NULL)
        on_status_button_clicked(GTK_BUTTON(status->power_button), NULL);
}

static gboolean on_control_panel_key_press(GtkWidget *widget,
                                           GdkEventKey *event,
                                           gpointer user_data) {
    (void)user_data;
    if (event->keyval == GDK_KEY_Escape) {
        gtk_widget_hide(widget);
        return TRUE;
    }
    return FALSE;
}

static gboolean hide_volume_osd(gpointer user_data) {
    gtk_widget_hide(GTK_WIDGET(user_data));
    status->osd_hide_source = 0;
    return G_SOURCE_REMOVE;
}

static gboolean hide_brightness_osd(gpointer user_data) {
    gtk_widget_hide(GTK_WIDGET(user_data));
    status->brightness_osd_hide_source = 0;
    return G_SOURCE_REMOVE;
}

static void show_volume_osd(void) {
    gboolean muted = FALSE;
    int volume = read_volume_percent(&muted);
    if (volume >= 0) {
        status->setting_scale = TRUE;
        gtk_range_set_value(GTK_RANGE(status->volume_scale), volume);
        status->setting_scale = FALSE;
    }
    int icon_volume = volume < 0 ? 50 : ((volume + 2) / 5) * 5;
    char icon_name[32];
    if (muted) {
        g_strlcpy(icon_name, "volume_mute.png", sizeof(icon_name));
    } else {
        g_snprintf(icon_name, sizeof(icon_name), "volume_%03d.png",
                   CLAMP(icon_volume, 0, 100));
    }
    set_asset_icon(status->volume_osd_icon, "volume", icon_name, 200);
    gtk_window_set_position(GTK_WINDOW(status->volume_osd), GTK_WIN_POS_CENTER);
    gtk_widget_show_all(status->volume_osd);
    gtk_window_present(GTK_WINDOW(status->volume_osd));
    if (status->osd_hide_source) g_source_remove(status->osd_hide_source);
    status->osd_hide_source =
        g_timeout_add(800, hide_volume_osd, status->volume_osd);
}

static gboolean is_wireless_interface(const char *interface) {
    char *path = g_build_filename("/sys/class/net", interface, "wireless", NULL);
    gboolean found = g_file_test(path, G_FILE_TEST_IS_DIR);
    g_free(path);
    return found;
}

static int wireless_strength(const char *interface) {
    FILE *file = fopen("/proc/net/wireless", "r");
    if (!file) return -1;
    char *line = NULL;
    size_t capacity = 0;
    int strength = -1;
    while (getline(&line, &capacity, file) >= 0) {
        char name[64];
        double link_quality;
        if (sscanf(line, " %63[^:]: %*x %lf", name, &link_quality) == 2 &&
            g_str_equal(name, interface)) {
            strength = CLAMP((int)(link_quality * 100.0 / 70.0), 0, 100);
            break;
        }
    }
    free(line);
    fclose(file);
    return strength;
}

static void update_network_icon(void) {
    GDir *directory = g_dir_open("/sys/class/net", 0, NULL);
    if (!directory) return;
    gboolean connected_wifi = FALSE;
    gboolean connected_ethernet = FALSE;
    int signal = -1;
    const char *interface;
    while ((interface = g_dir_read_name(directory))) {
        if (g_str_equal(interface, "lo")) continue;
        char *state_path = g_build_filename("/sys/class/net", interface,
                                            "operstate", NULL);
        char *operstate = NULL;
        g_file_get_contents(state_path, &operstate, NULL, NULL);
        gboolean connected = operstate && g_str_has_prefix(operstate, "up");
        g_free(operstate);
        g_free(state_path);
        if (!connected) continue;
        if (is_wireless_interface(interface)) {
            connected_wifi = TRUE;
            signal = wireless_strength(interface);
        } else {
            connected_ethernet = TRUE;
        }
    }
    g_dir_close(directory);

    const char *icon;
    const char *tooltip;
    if (connected_wifi) {
        if (signal >= 80) icon = "network-wireless-signal-excellent-symbolic.png";
        else if (signal >= 55) icon = "network-wireless-signal-good-symbolic.png";
        else if (signal >= 30) icon = "network-wireless-signal-ok-symbolic.png";
        else if (signal >= 0) icon = "network-wireless-signal-weak-symbolic.png";
        else icon = "network-wireless-signal-none-symbolic.png";
        tooltip = signal >= 0 ? "Wi-Fi connected" : "Wi-Fi connected; signal unavailable";
    } else if (connected_ethernet) {
        icon = "wire-pluged-in.png";
        tooltip = "Wired network connected";
    } else {
        icon = "network-wireless-offline-symbolic.png";
        tooltip = "Network disconnected";
    }
    gtk_label_set_text(GTK_LABEL(status->network_detail), tooltip);
    set_asset_icon(gtk_button_get_image(GTK_BUTTON(status->wifi_button)),
                   "icons/ethernet", icon, 18);
    gtk_widget_set_tooltip_text(status->wifi_button, tooltip);
}

static void update_battery_icon(void) {
    GDir *directory = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!directory) {
        gtk_label_set_text(GTK_LABEL(status->battery_detail),
                           "Battery status unavailable");
        gtk_widget_hide(status->battery_button);
        return;
    }
    gboolean found = FALSE;
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        char *type_path = g_build_filename("/sys/class/power_supply", name,
                                           "type", NULL);
        char *type = NULL;
        g_file_get_contents(type_path, &type, NULL, NULL);
        g_free(type_path);
        if (!type || !g_str_equal(g_strstrip(type), "Battery")) {
            g_free(type);
            continue;
        }
        g_free(type);

        char *capacity_path = g_build_filename("/sys/class/power_supply", name,
                                               "capacity", NULL);
        char *capacity_text = NULL;
        g_file_get_contents(capacity_path, &capacity_text, NULL, NULL);
        g_free(capacity_path);
        if (!capacity_text) continue;
        int capacity = CLAMP((int)g_ascii_strtoll(capacity_text, NULL, 10), 0, 100);
        g_free(capacity_text);

        char *state_path = g_build_filename("/sys/class/power_supply", name,
                                            "status", NULL);
        char *state = NULL;
        g_file_get_contents(state_path, &state, NULL, NULL);
        g_free(state_path);
        gboolean charging = state &&
            (g_str_has_prefix(state, "Charging") || g_str_has_prefix(state, "Full"));
        gboolean full = state && g_str_has_prefix(state, "Full");
        g_free(state);
        char icon[64];
        if (full) {
            g_strlcpy(icon, "battery-full-charged-symbolic.png", sizeof(icon));
        } else {
            int level = MIN(100, ((capacity + 5) / 10) * 10);
            g_snprintf(icon, sizeof(icon), "battery-level-%d-%ssymbolic.png",
                       level, charging ? "charging-" : "");
        }
        GtkWidget *image = gtk_button_get_image(GTK_BUTTON(status->battery_button));
        set_asset_icon(image, "icons/battery", icon, 18);
        char *tooltip = g_strdup_printf("%s%s%d%%",
            charging ? "Charging: " : "Battery: ", "", capacity);
        gtk_label_set_text(GTK_LABEL(status->battery_detail), tooltip);
        gtk_widget_set_tooltip_text(status->battery_button, tooltip);
        g_free(tooltip);
        gtk_widget_show(status->battery_button);
        found = TRUE;
        break;
    }
    g_dir_close(directory);
    if (!found) {
        gtk_label_set_text(GTK_LABEL(status->battery_detail),
                           "No battery detected");
        gtk_widget_hide(status->battery_button);
    }
}

static void on_wifi_settings_clicked(GtkButton *button,
                                     gpointer user_data) {
    (void)button;
    (void)user_data;
    static const struct {
        const char *program;
        const char *argument;
    } candidates[] = {
        { "nm-connection-editor", NULL },
        { "gnome-control-center", "network" },
        { "systemsettings", "kcm_networkmanagement" },
        { "xfce4-settings-manager", NULL },
        { NULL, NULL }
    };

    for (int i = 0; candidates[i].program; i++) {
        char *program = g_find_program_in_path(candidates[i].program);
        if (!program) continue;
        const char *argv[] = { program, candidates[i].argument, NULL };
        launch_control_command(argv);
        g_free(program);
        return;
    }

    GtkWidget *parent = gtk_widget_get_toplevel(status->wifi_button);
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_IS_WINDOW(parent) ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "No suitable app was found for Wi-Fi control.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static gboolean poll_system_status(gpointer user_data) {
    (void)user_data;
    update_network_icon();
    update_battery_icon();

    gboolean muted;
    int volume = read_volume_percent(&muted);
    if (volume >= 0) {
        gtk_widget_set_tooltip_text(status->volume_scale, NULL);
        status->volume_muted = muted;
        gtk_button_set_label(GTK_BUTTON(status->mute_button),
                             muted ? "Unmute" : "Mute");
        status->setting_scale = TRUE;
        gtk_range_set_value(GTK_RANGE(status->volume_scale), volume);
        status->setting_scale = FALSE;
    } else if (status->volume_backend) {
        char *tooltip = g_strdup_printf(
            "Could not read audio state using %s", status->volume_backend);
        gtk_widget_set_tooltip_text(status->volume_scale, tooltip);
        g_free(tooltip);
    }
    int brightness = read_brightness_percent();
    if (brightness >= 0) {
        gtk_widget_set_tooltip_text(
            status->brightness_scale,
            backlight_is_writable() ? NULL :
            "Brightness is read-only for this user. Install macOS-DE's backlight access rule, then log out and back in.");
        status->setting_scale = TRUE;
        gtk_range_set_value(GTK_RANGE(status->brightness_scale), brightness);
        status->setting_scale = FALSE;
    } else if (status->brightness_backend) {
        char *tooltip = g_strdup_printf(
            "Could not read brightness using %s",
            status->brightness_backend);
        gtk_widget_set_tooltip_text(status->brightness_scale, tooltip);
        g_free(tooltip);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean poll_volume_event(gpointer user_data) {
    (void)user_data;
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Atom event_atom = XInternAtom(display, "_MACOS_DE_VOLUME_EVENT", False);
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    if (XGetWindowProperty(display, DefaultRootWindow(display), event_atom,
                           0, 1, False, XA_CARDINAL, &actual_type,
                           &actual_format, &count, &bytes_after, &value) == Success &&
        value && actual_type == XA_CARDINAL && actual_format == 32 && count) {
        gulong current_event = (gulong)((unsigned long *)value)[0];
        if (current_event != status->volume_event) {
            status->volume_event = current_event;
            show_volume_osd();
        }
    }
    if (value) XFree(value);

    Atom brightness_event_atom = XInternAtom(
        display, "_MACOS_DE_BRIGHTNESS_EVENT", False);
    unsigned char *brightness_value = NULL;
    if (XGetWindowProperty(display, DefaultRootWindow(display),
                           brightness_event_atom, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &count, &bytes_after,
                           &brightness_value) == Success &&
        brightness_value && actual_type == XA_CARDINAL &&
        actual_format == 32 && count) {
        gulong current_event =
            (gulong)((unsigned long *)brightness_value)[0];
        if (current_event != status->brightness_event) {
            status->brightness_event = current_event;
            show_brightness_osd();
        }
    }
    if (brightness_value) XFree(brightness_value);
    return G_SOURCE_CONTINUE;
}

static void show_brightness_osd(void) {
    int brightness = read_brightness_percent();
    if (brightness < 0)
        return;

    status->setting_scale = TRUE;
    gtk_range_set_value(GTK_RANGE(status->brightness_scale), brightness);
    status->setting_scale = FALSE;

    int icon_brightness = ((brightness + 2) / 5) * 5;
    char icon_name[32];
    g_snprintf(icon_name, sizeof(icon_name), "brightness_%03d.png",
               CLAMP(icon_brightness, 0, 100));
    set_asset_icon(status->brightness_osd_icon, "brightness", icon_name, 200);
    gtk_window_set_position(GTK_WINDOW(status->brightness_osd),
                            GTK_WIN_POS_CENTER);
    gtk_widget_show_all(status->brightness_osd);
    gtk_window_present(GTK_WINDOW(status->brightness_osd));
    if (status->brightness_osd_hide_source)
        g_source_remove(status->brightness_osd_hide_source);
    status->brightness_osd_hide_source =
        g_timeout_add(800, hide_brightness_osd, status->brightness_osd);
}

static GtkWidget *create_scale_row(const char *icon, const char *label,
                                   GtkWidget **scale_out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    GtkWidget *name = gtk_label_new(label);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_size_request(scale, 190, -1);
    gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    *scale_out = scale;
    return row;
}

GtkWidget *system_status_create(GtkWidget *power_button,
                                GCallback logout_callback,
                                GCallback reboot_callback,
                                GCallback shutdown_callback,
                                const char *asset_directory,
                                gboolean dark_theme) {
    status = g_new0(SystemStatus, 1);
    status->power_button = power_button;
    status->asset_directory = g_strdup(asset_directory);
    status->dark_theme = dark_theme;
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Atom volume_event_atom = XInternAtom(
        display, "_MACOS_DE_VOLUME_EVENT", False);
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long bytes_after;
    unsigned char *volume_event = NULL;
    if (XGetWindowProperty(display, DefaultRootWindow(display),
                           volume_event_atom, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &count, &bytes_after,
                           &volume_event) == Success &&
        volume_event && actual_type == XA_CARDINAL &&
        actual_format == 32 && count) {
        status->volume_event = (gulong)((unsigned long *)volume_event)[0];
    }
    if (volume_event) XFree(volume_event);
    Atom brightness_event_atom = XInternAtom(
        display, "_MACOS_DE_BRIGHTNESS_EVENT", False);
    unsigned char *brightness_event = NULL;
    if (XGetWindowProperty(display, DefaultRootWindow(display),
                           brightness_event_atom, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &count, &bytes_after,
                           &brightness_event) == Success &&
        brightness_event && actual_type == XA_CARDINAL &&
        actual_format == 32 && count) {
        status->brightness_event =
            (gulong)((unsigned long *)brightness_event)[0];
    }
    if (brightness_event) XFree(brightness_event);
    static const char *const volume_programs[] = {
        "wpctl", "pactl", "amixer", NULL
    };
    static const char *const brightness_programs[] = {
        "brightnessctl", "xbacklight", NULL
    };
    status->volume_backend = find_program(volume_programs);
    status->brightness_device = find_backlight_device();
    if (status->brightness_device == NULL)
        status->brightness_backend = find_program(brightness_programs + 1);

    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    status->wifi_button = gtk_button_new();
    status->battery_button = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(status->wifi_button),
                                "topbar-status-button");
    gtk_style_context_add_class(gtk_widget_get_style_context(status->battery_button),
                                "topbar-status-button");
    gtk_button_set_image(GTK_BUTTON(status->wifi_button),
                         gtk_image_new_from_icon_name("network-wireless",
                                                      GTK_ICON_SIZE_MENU));
    gtk_button_set_image(GTK_BUTTON(status->battery_button),
                         gtk_image_new_from_icon_name("battery",
                                                      GTK_ICON_SIZE_MENU));
    gtk_box_pack_start(GTK_BOX(status_box), status->wifi_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_box), status->battery_button, FALSE, FALSE, 0);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(content, 14);
    gtk_widget_set_margin_end(content, 14);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);

    GtkWidget *network_row = create_status_row(
        "network-wireless", "Network disconnected", &status->network_detail);
    GtkWidget *battery_row = create_status_row(
        "battery", "Battery status unavailable", &status->battery_detail);
    gtk_box_pack_start(GTK_BOX(content), network_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), battery_row, FALSE, FALSE, 0);
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content), separator, FALSE, FALSE, 0);

    GtkWidget *volume_row = create_scale_row("audio-volume-high-symbolic",
                                             "Volume", &status->volume_scale);
    gtk_box_pack_start(GTK_BOX(content), volume_row, FALSE, FALSE, 0);
    GtkWidget *brightness_row = create_scale_row("display-brightness-symbolic",
        "Brightness", &status->brightness_scale);
    gtk_box_pack_start(GTK_BOX(content), brightness_row, FALSE, FALSE, 0);
    status->mute_button = gtk_button_new_with_label("Mute");
    gtk_widget_set_tooltip_text(status->mute_button,
                                "Audio keys or Ctrl+Alt+M");
    gtk_widget_set_halign(status->mute_button, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(content), status->mute_button, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(status->volume_scale, status->volume_backend != NULL);
    gtk_widget_set_sensitive(status->mute_button,
                              status->volume_backend != NULL);
    gtk_widget_set_tooltip_text(status->volume_scale,
        status->volume_backend ? NULL : "Install wpctl, pactl, or amixer");
    gboolean brightness_writable = backlight_is_writable();
    gtk_widget_set_sensitive(status->brightness_scale, brightness_writable);
    gtk_widget_set_tooltip_text(status->brightness_scale,
        brightness_writable ? NULL :
        status->brightness_device
            ? "Brightness is read-only for this user. Install macOS-DE's backlight access rule, then log out and back in."
            : "No controllable display backlight or brightness utility found");

    GtkWidget *power_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(power_row),
        create_power_button("Log Out", "system-log-out", logout_callback),
        TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(power_row),
        create_power_button("Reboot", "system-reboot", reboot_callback),
        TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(power_row),
        create_power_button("Shut Down", "system-shutdown", shutdown_callback),
        TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), power_row, FALSE, FALSE, 0);
    status->control_panel = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_size_request(status->control_panel, 340, -1);
    prepare_transparent_window(status->control_panel);
    gtk_window_set_decorated(GTK_WINDOW(status->control_panel), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(status->control_panel), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(status->control_panel), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(status->control_panel), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(status->control_panel),
                             GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_accept_focus(GTK_WINDOW(status->control_panel), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(status->control_panel),
        "system-status-panel");
    gtk_container_add(GTK_CONTAINER(status->control_panel), content);
    gtk_widget_show_all(content);
    gtk_widget_add_events(status->control_panel, GDK_KEY_PRESS_MASK);
    g_signal_connect(status->control_panel, "key-press-event",
                     G_CALLBACK(on_control_panel_key_press), NULL);

    status->volume_osd = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    prepare_transparent_window(status->volume_osd);
    gtk_window_set_decorated(GTK_WINDOW(status->volume_osd), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(status->volume_osd), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(status->volume_osd), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(status->volume_osd), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(status->volume_osd),
                             GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(status->volume_osd), "volume-osd");
    status->volume_osd_icon = gtk_image_new();
    gtk_widget_set_size_request(status->volume_osd_icon, 200, 200);
    gtk_container_add(GTK_CONTAINER(status->volume_osd), status->volume_osd_icon);

    status->brightness_osd = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    prepare_transparent_window(status->brightness_osd);
    gtk_window_set_decorated(GTK_WINDOW(status->brightness_osd), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(status->brightness_osd), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(status->brightness_osd), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(status->brightness_osd), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(status->brightness_osd),
                             GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(status->brightness_osd), "brightness-osd");
    status->brightness_osd_icon = gtk_image_new();
    gtk_widget_set_size_request(status->brightness_osd_icon, 200, 200);
    gtk_container_add(GTK_CONTAINER(status->brightness_osd),
                      status->brightness_osd_icon);

    g_signal_connect(status->volume_scale, "value-changed",
                     G_CALLBACK(on_volume_changed), NULL);
    g_signal_connect(status->brightness_scale, "value-changed",
                     G_CALLBACK(on_brightness_changed), NULL);
    g_signal_connect(status->mute_button, "clicked",
                     G_CALLBACK(on_volume_mute_clicked), NULL);
    g_signal_connect(status->wifi_button, "clicked",
                     G_CALLBACK(on_wifi_settings_clicked), NULL);
    g_signal_connect(power_button, "clicked",
                     G_CALLBACK(on_status_button_clicked), NULL);

    gtk_widget_show_all(status_box);
    poll_system_status(NULL);
    g_timeout_add_seconds(2, poll_system_status, NULL);
    g_timeout_add(60, poll_volume_event, NULL);
    return status_box;
}
