#define _POSIX_C_SOURCE 200809L
#include "macoswm.h"
#include "client_decorations.h"

#include <X11/Xcursor/Xcursor.h>
#include <X11/Xft/Xft.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TITLEBAR_HEIGHT 30
#define FRAME_BORDER 1
#define BUTTON_SIZE 12
#define BUTTON_TOP 9
#define BUTTON_LEFT 12
#define BUTTON_GAP 8
#define DOUBLE_CLICK_MS 300
#define RESIZE_MARGIN 8
#define ROUNDED_CORNER_RESIZE_MARGIN 24
#define SQUARE_CORNER_RESIZE_MARGIN 16

enum {
    RESIZE_LEFT = 1,
    RESIZE_RIGHT = 2,
    RESIZE_TOP = 4,
    RESIZE_BOTTOM = 8
};

typedef struct ManagedWindow ManagedWindow;

struct ManagedWindow {
    Window client;
    Window frame;
    Window titlebar;
    Window buttons[3];
    Window resize_handles[8];
    int x;
    int y;
    int width;
    int height;
    int restore_x;
    int restore_y;
    int restore_width;
    int restore_height;
    int border_width;
    int maximized;
    int minimized;
    int fullscreen;
    int client_decorated;
    ManagedWindow *next;
};

typedef struct {
    Atom wm_protocols;
    Atom wm_delete_window;
    Atom wm_state;
    Atom wm_change_state;
    Atom net_supported;
    Atom net_supporting_wm_check;
    Atom net_wm_name;
    Atom net_wm_state;
    Atom net_wm_state_maximized_horz;
    Atom net_wm_state_maximized_vert;
    Atom net_wm_state_fullscreen;
    Atom net_active_window;
    Atom net_wm_moveresize;
    Atom net_client_list;
    Atom net_wm_window_type;
    Atom net_wm_window_type_desktop;
    Atom net_wm_window_type_dock;
    Atom net_wm_window_type_splash;
    Atom net_wm_window_type_dialog;
    Atom net_wm_window_type_menu;
    Atom net_wm_window_type_dropdown_menu;
    Atom net_wm_window_type_popup_menu;
    Atom net_wm_window_type_tooltip;
    Atom net_wm_window_type_notification;
    Atom net_wm_window_type_dnd;
    Atom net_wm_strut_partial;
    Atom macos_de_volume_event;
} WmAtoms;

struct MacosWm {
    Display *display;
    int screen;
    Window root;
    Window support_window;
    GC gc;
    XFontStruct *font;
    XftFont *xft_font;
    XftColor xft_text_color;
    int xft_text_color_allocated;
    Cursor normal_cursor;
    Cursor move_cursor;
    Cursor horizontal_cursor;
    Cursor vertical_cursor;
    Cursor diagonal_cursor[2];
    Cursor working_cursors[15];
    Cursor busy_cursors[15];
    int animation_frame;
    unsigned long title_color;
    unsigned long border_color;
    unsigned long button_colors[3];
    unsigned long button_hover_colors[3];
    unsigned long text_color;
    WmAtoms atoms;
    ManagedWindow *windows;
    ManagedWindow *active;
    ManagedWindow *drag_window;
    ManagedWindow *switcher_selection;
    Window switcher_window;
    long long switcher_hide_at_ms;
    int drag_root_x;
    int drag_root_y;
    int drag_window_x;
    int drag_window_y;
    int drag_window_width;
    int drag_window_height;
    int resizing;
    int resize_edges;
    unsigned int drag_button;
    int drag_pointer_grabbed;
    Time last_title_click;
    int running;
    int switcher_visible;
    int dock_hidden;
    int panel_hidden;
    int shape_extension_available;
    int circular_buttons;
    int rounded_corners;
    int settings_mtime_valid;
    struct timespec settings_mtime;
    long long heartbeat_at_ms;
    unsigned long volume_event_serial;
};

static MacosWm *error_handler_wm;
static int x11_error_code;

static Cursor cursor_for_resize_edges(MacosWm *wm, int edges);
static int resize_handle_edges(int handle);
static void update_resize_handles(MacosWm *wm, ManagedWindow *managed);
static void draw_titlebar(MacosWm *wm, ManagedWindow *managed);
static void begin_window_drag(MacosWm *wm, ManagedWindow *managed,
                              XButtonEvent *event, int resizing,
                              int resize_edges);

static int titlebar_height(const ManagedWindow *managed) {
    return managed->fullscreen || managed->client_decorated
        ? 0 : TITLEBAR_HEIGHT;
}

static char *cursor_asset_path(const char *filename) {
    const char *directories[] = {
        getenv("MACOS_DE_CURSOR_DIR"),
        "/usr/share/macos-de/assets/system/cursor/default1",
        "build/assets/system/cursor/default1",
        "assets/system/cursor/default1",
        NULL
    };
    for (int i = 0; directories[i]; i++) {
        if (!directories[i] || !*directories[i]) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", directories[i], filename);
        if (access(path, R_OK) == 0) return strdup(path);
    }
    return NULL;
}

static char *font_asset_path(void) {
    const char *paths[] = {
        "/usr/share/macos-de/assets/system/fonts/SF/SFPRODISPLAYBLACKITALIC.OTF",
        "assets/system/fonts/SF/SFPRODISPLAYBLACKITALIC.OTF",
        "build/assets/system/fonts/SF/SFPRODISPLAYBLACKITALIC.OTF",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], R_OK) == 0) return strdup(paths[i]);
    }
    return NULL;
}

static Cursor load_cursor_asset(MacosWm *wm, const char *filename,
                                unsigned int hotspot_x,
                                unsigned int hotspot_y) {
    char *path = cursor_asset_path(filename);
    if (!path) return None;
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &error);
    free(path);
    if (!pixbuf) {
        if (error) {
            fprintf(stderr, "macos-wm: could not load cursor image: %s\n",
                    error->message);
            g_error_free(error);
        }
        return None;
    }

    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
    XcursorImage *image = XcursorImageCreate((int)width, (int)height);
    if (!image) {
        g_object_unref(pixbuf);
        return None;
    }
    image->xhot = MIN(hotspot_x, (unsigned int)width - 1);
    image->yhot = MIN(hotspot_y, (unsigned int)height - 1);
    for (int y = 0; y < height; y++) {
        const guchar *row = pixels + y * stride;
        for (int x = 0; x < width; x++) {
            const guchar *pixel = row + x * channels;
            unsigned int red = pixel[0];
            unsigned int green = channels >= 3 ? pixel[1] : red;
            unsigned int blue = channels >= 3 ? pixel[2] : red;
            unsigned int alpha = channels == 2 ? pixel[1] :
                                 channels == 4 ? pixel[3] : 255;
            image->pixels[y * width + x] =
                (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
    }
    Cursor cursor = XcursorImageLoadCursor(wm->display, image);
    XcursorImageDestroy(image);
    g_object_unref(pixbuf);
    return cursor;
}

static void load_wm_cursors(MacosWm *wm) {
    wm->normal_cursor = load_cursor_asset(wm, "Normal.png", 0, 0);
    wm->move_cursor = load_cursor_asset(wm, "Bonus-Move.png", 16, 16);
    wm->horizontal_cursor =
        load_cursor_asset(wm, "Horizontal-Resize.png", 16, 16);
    wm->vertical_cursor =
        load_cursor_asset(wm, "Vertical Resize.png", 16, 16);
    wm->diagonal_cursor[0] =
        load_cursor_asset(wm, "Diagonal-Resize-1.png", 16, 16);
    wm->diagonal_cursor[1] =
        load_cursor_asset(wm, "Diagonal-Resize-2.png", 16, 16);
    for (int i = 0; i < 15; i++) {
        char path[64];
        snprintf(path, sizeof(path), "Working/Working_page_%02d.png", i + 1);
        wm->working_cursors[i] =
            load_cursor_asset(wm, path, 0, 0);
        snprintf(path, sizeof(path), "Busy/Busy_page_%02d.png", i + 1);
        wm->busy_cursors[i] = load_cursor_asset(wm, path, 0, 0);
    }
    if (wm->normal_cursor != None) {
        XDefineCursor(wm->display, wm->root, wm->normal_cursor);
    }
}

static int wm_error_handler(Display *display, XErrorEvent *event) {
    (void)display;
    if (event->error_code == BadAccess) {
        x11_error_code = BadAccess;
        return 0;
    }

    if (error_handler_wm) {
        char message[128];
        XGetErrorText(display, event->error_code, message, sizeof(message));
        fprintf(stderr, "macos-wm: X11 error: %s (request %u.%u)\n",
                message, event->request_code, event->minor_code);
    }
    return 0;
}

static Atom intern_atom(Display *display, const char *name) {
    return XInternAtom(display, name, False);
}

static void initialize_atoms(MacosWm *wm) {
    Display *display = wm->display;
    WmAtoms *a = &wm->atoms;
    a->wm_protocols = intern_atom(display, "WM_PROTOCOLS");
    a->wm_delete_window = intern_atom(display, "WM_DELETE_WINDOW");
    a->wm_state = intern_atom(display, "WM_STATE");
    a->wm_change_state = intern_atom(display, "WM_CHANGE_STATE");
    a->net_supported = intern_atom(display, "_NET_SUPPORTED");
    a->net_supporting_wm_check = intern_atom(display, "_NET_SUPPORTING_WM_CHECK");
    a->net_wm_name = intern_atom(display, "_NET_WM_NAME");
    a->net_wm_state = intern_atom(display, "_NET_WM_STATE");
    a->net_wm_state_maximized_horz = intern_atom(display, "_NET_WM_STATE_MAXIMIZED_HORZ");
    a->net_wm_state_maximized_vert = intern_atom(display, "_NET_WM_STATE_MAXIMIZED_VERT");
    a->net_wm_state_fullscreen = intern_atom(display, "_NET_WM_STATE_FULLSCREEN");
    a->net_active_window = intern_atom(display, "_NET_ACTIVE_WINDOW");
    a->net_wm_moveresize = intern_atom(display, "_NET_WM_MOVERESIZE");
    a->net_client_list = intern_atom(display, "_NET_CLIENT_LIST");
    a->net_wm_window_type = intern_atom(display, "_NET_WM_WINDOW_TYPE");
    a->net_wm_window_type_desktop = intern_atom(display, "_NET_WM_WINDOW_TYPE_DESKTOP");
    a->net_wm_window_type_dock = intern_atom(display, "_NET_WM_WINDOW_TYPE_DOCK");
    a->net_wm_window_type_splash = intern_atom(display, "_NET_WM_WINDOW_TYPE_SPLASH");
    a->net_wm_window_type_dialog = intern_atom(display, "_NET_WM_WINDOW_TYPE_DIALOG");
    a->net_wm_window_type_menu = intern_atom(display, "_NET_WM_WINDOW_TYPE_MENU");
    a->net_wm_window_type_dropdown_menu = intern_atom(display, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
    a->net_wm_window_type_popup_menu = intern_atom(display, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
    a->net_wm_window_type_tooltip = intern_atom(display, "_NET_WM_WINDOW_TYPE_TOOLTIP");
    a->net_wm_window_type_notification = intern_atom(display, "_NET_WM_WINDOW_TYPE_NOTIFICATION");
    a->net_wm_window_type_dnd = intern_atom(display, "_NET_WM_WINDOW_TYPE_DND");
    a->net_wm_strut_partial = intern_atom(display, "_NET_WM_STRUT_PARTIAL");
    a->macos_de_volume_event =
        intern_atom(display, "_MACOS_DE_VOLUME_EVENT");
}

static int program_in_path(const char *program) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    char *save = NULL;
    for (char *directory = strtok_r(copy, ":", &save); directory;
         directory = strtok_r(NULL, ":", &save)) {
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/%s", directory, program);
        if (access(candidate, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

static void run_volume_command(MacosWm *wm, KeySym key) {
    int direction = key == XF86XK_AudioRaiseVolume ? 1 :
                    key == XF86XK_AudioLowerVolume ? -1 : 0;
    const char *backend = program_in_path("wpctl") ? "wpctl" :
                          program_in_path("pactl") ? "pactl" :
                          program_in_path("amixer") ? "amixer" : NULL;
    if (!backend) return;

    char *argv[6] = {0};
    if (strcmp(backend, "wpctl") == 0) {
        argv[0] = (char *)backend;
        argv[1] = direction ? "set-volume" : "set-mute";
        argv[2] = "@DEFAULT_AUDIO_SINK@";
        argv[3] = direction == 1 ? "5%+" :
                  direction == -1 ? "5%-" : "toggle";
    } else if (strcmp(backend, "pactl") == 0) {
        argv[0] = (char *)backend;
        argv[1] = direction ? "set-sink-volume" : "set-sink-mute";
        argv[2] = "@DEFAULT_SINK@";
        argv[3] = direction == 1 ? "+5%" :
                  direction == -1 ? "-5%" : "toggle";
    } else {
        argv[0] = (char *)backend;
        argv[1] = "-q";
        argv[2] = "sset";
        argv[3] = "Master";
        argv[4] = direction == 1 ? "5%+" :
                  direction == -1 ? "5%-" : "toggle";
    }

    pid_t child;
    int result = posix_spawnp(&child, backend, NULL, NULL, argv, environ);
    if (result != 0) {
        fprintf(stderr, "macos-wm: could not run %s for volume control: %s\n",
                backend, strerror(result));
        return;
    }

    wm->volume_event_serial++;
    XChangeProperty(wm->display, wm->root,
                    wm->atoms.macos_de_volume_event,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&wm->volume_event_serial, 1);
}

static void run_screenshot_manager(const char *mode) {
    char executable_path[PATH_MAX + 1];
    char sibling_candidate[PATH_MAX + 1];
    const char *candidates[3] = {
        NULL,
        "/usr/share/macos-de/apps/screenshoot-manager/screenshoot-manager",
        "build/apps/screenshoot-manager/screenshoot-manager"
    };
    ssize_t length = readlink("/proc/self/exe", executable_path,
                              sizeof(executable_path) - 1);
    if (length >= 0) {
        executable_path[length] = '\0';
        char *separator = strrchr(executable_path, '/');
        if (separator) {
            *separator = '\0';
            int written = snprintf(
                sibling_candidate, sizeof(sibling_candidate),
                "%s/../apps/screenshoot-manager/screenshoot-manager",
                executable_path);
            if (written > 0 && (size_t)written < sizeof(sibling_candidate)) {
                candidates[0] = sibling_candidate;
            }
        }
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *path = candidates[i];
        if (!path || access(path, X_OK) != 0) continue;
        char *argv[] = { (char *)path, (char *)mode, NULL };
        pid_t child;
        int result = posix_spawn(&child, path, NULL, NULL, argv, environ);
        if (result != 0) {
            fprintf(stderr, "macos-wm: could not start screenshot manager: %s\n",
                    strerror(result));
        }
        return;
    }

    char *path_argv[] = { "screenshoot-manager", (char *)mode, NULL };
    pid_t child;
    int result = posix_spawnp(&child, path_argv[0], NULL, NULL, path_argv, environ);
    if (result == 0) return;
    if (result != ENOENT && result != ENOTDIR) {
        fprintf(stderr, "macos-wm: could not start screenshoot-manager from PATH: %s\n",
                strerror(result));
        return;
    }

    fprintf(stderr, "macos-wm: screenshoot-manager was not found; build macOS-DE, "
                    "install it under /usr/share/macos-de, or add its bin directory to PATH\n");
}

static int is_volume_keysym(KeySym key) {
    return key == XF86XK_AudioRaiseVolume ||
           key == XF86XK_AudioLowerVolume ||
           key == XF86XK_AudioMute;
}

static int is_brightness_keysym(KeySym key) {
    return key == XF86XK_MonBrightnessUp ||
           key == XF86XK_MonBrightnessDown;
}

static KeySym volume_keysym_for_keycode(Display *display, KeyCode keycode) {
    int symbols_per_keycode = 0;
    KeySym *symbols = XGetKeyboardMapping(display, keycode, 1,
                                          &symbols_per_keycode);
    KeySym result = NoSymbol;
    for (int i = 0; symbols && i < symbols_per_keycode; i++) {
        if (is_volume_keysym(symbols[i])) {
            result = symbols[i];
            break;
        }
    }
    if (symbols) XFree(symbols);
    return result;
}

static KeySym brightness_keysym_for_keycode(Display *display,
                                            KeyCode keycode) {
    int symbols_per_keycode = 0;
    KeySym *symbols = XGetKeyboardMapping(display, keycode, 1,
                                          &symbols_per_keycode);
    KeySym result = NoSymbol;
    for (int i = 0; symbols && i < symbols_per_keycode; i++) {
        if (is_brightness_keysym(symbols[i])) {
            result = symbols[i];
            break;
        }
    }
    if (symbols) XFree(symbols);
    return result;
}

static void run_brightness_command(KeySym key) {
    DIR *directory = opendir("/sys/class/backlight");
    if (directory == NULL) {
        fprintf(stderr, "macos-wm: could not access display backlight: %s\n",
                strerror(errno));
        return;
    }

    struct dirent *entry;
    int handled = 0;
    while (!handled && (entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char brightness_path[PATH_MAX];
        char maximum_path[PATH_MAX];
        int brightness_length = snprintf(
            brightness_path, sizeof(brightness_path),
            "/sys/class/backlight/%s/brightness", entry->d_name);
        int maximum_length = snprintf(
            maximum_path, sizeof(maximum_path),
            "/sys/class/backlight/%s/max_brightness", entry->d_name);
        if (brightness_length < 0 ||
            (size_t)brightness_length >= sizeof(brightness_path) ||
            maximum_length < 0 ||
            (size_t)maximum_length >= sizeof(maximum_path))
            continue;

        FILE *brightness_file = fopen(brightness_path, "r");
        FILE *maximum_file = fopen(maximum_path, "r");
        long current = -1, maximum = -1;
        if (brightness_file != NULL) {
            if (fscanf(brightness_file, "%ld", &current) != 1)
                current = -1;
            fclose(brightness_file);
        }
        if (maximum_file != NULL) {
            if (fscanf(maximum_file, "%ld", &maximum) != 1)
                maximum = -1;
            fclose(maximum_file);
        }
        if (current < 0 || maximum <= 0)
            continue;

        long step = MAX(1L, maximum / 20);
        long updated = key == XF86XK_MonBrightnessUp
            ? MIN(maximum, current + step)
            : MAX(0L, current - step);
        char value[32];
        int value_length = snprintf(value, sizeof(value), "%ld", updated);
        int fd = open(brightness_path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "macos-wm: cannot write display brightness at %s: %s\n",
                    brightness_path, strerror(errno));
            handled = 1;
            break;
        }
        ssize_t written;
        do {
            written = write(fd, value, (size_t)value_length);
        } while (written < 0 && errno == EINTR);
        if (written != value_length) {
            int write_error = written < 0 ? errno : EIO;
            fprintf(stderr, "macos-wm: could not update display brightness at %s: %s\n",
                    brightness_path, strerror(write_error));
        }
        close(fd);
        handled = 1;
    }

    closedir(directory);
    if (!handled)
        fprintf(stderr, "macos-wm: no usable display backlight device was found\n");
}

static void grab_volume_keys(MacosWm *wm) {
    int minimum_keycode;
    int maximum_keycode;
    XDisplayKeycodes(wm->display, &minimum_keycode, &maximum_keycode);
    for (int keycode = minimum_keycode; keycode <= maximum_keycode; keycode++) {
        if (is_volume_keysym(volume_keysym_for_keycode(
                wm->display, (KeyCode)keycode))) {
            XGrabKey(wm->display, keycode, AnyModifier, wm->root, False,
                     GrabModeAsync, GrabModeAsync);
        }
    }

    const KeySym fallbacks[] = { XK_Up, XK_Down, XK_m, XK_M };
    const unsigned int lock_masks[] = {
        0, LockMask, Mod2Mask, LockMask | Mod2Mask
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        KeyCode keycode = XKeysymToKeycode(wm->display, fallbacks[i]);
        if (keycode == 0) continue;
        for (size_t j = 0; j < sizeof(lock_masks) / sizeof(lock_masks[0]); j++) {
            XGrabKey(wm->display, keycode, ControlMask | Mod1Mask | lock_masks[j],
                     wm->root, False, GrabModeAsync, GrabModeAsync);
        }
    }
}

static void grab_brightness_keys(MacosWm *wm) {
    int minimum_keycode;
    int maximum_keycode;
    XDisplayKeycodes(wm->display, &minimum_keycode, &maximum_keycode);
    for (int keycode = minimum_keycode; keycode <= maximum_keycode; keycode++) {
        if (is_brightness_keysym(brightness_keysym_for_keycode(
                wm->display, (KeyCode)keycode))) {
            XGrabKey(wm->display, keycode, AnyModifier, wm->root, False,
                     GrabModeAsync, GrabModeAsync);
        }
    }
}

static unsigned long named_color(Display *display, const char *name,
                                 unsigned long fallback) {
    XColor color;
    Colormap colormap = DefaultColormap(display, DefaultScreen(display));
    if (XAllocNamedColor(display, colormap, name, &color, &color)) return color.pixel;
    return fallback;
}

static void setup_colors(MacosWm *wm) {
    unsigned long white = WhitePixel(wm->display, wm->screen);
    wm->title_color = named_color(wm->display, "#e8e8ed", white);
    wm->border_color = named_color(wm->display, "#b8b8bf", BlackPixel(wm->display, wm->screen));
    wm->text_color = named_color(wm->display, "#242428", BlackPixel(wm->display, wm->screen));
    wm->button_colors[0] = named_color(wm->display, "#ff5f57", wm->border_color);
    wm->button_colors[1] = named_color(wm->display, "#febc2e", wm->border_color);
    wm->button_colors[2] = named_color(wm->display, "#28c840", wm->border_color);
    wm->button_hover_colors[0] = named_color(wm->display, "#e0443e", wm->button_colors[0]);
    wm->button_hover_colors[1] = named_color(wm->display, "#dea123", wm->button_colors[1]);
    wm->button_hover_colors[2] = named_color(wm->display, "#1eaa35", wm->button_colors[2]);
}

static void apply_button_shape(MacosWm *wm, Window button) {
    if (!wm->shape_extension_available || button == None) return;
    if (!wm->circular_buttons) {
        XShapeCombineMask(wm->display, button, ShapeBounding, 0, 0,
                          None, ShapeSet);
        return;
    }

    static const short left_edges[BUTTON_SIZE] = {
        3, 2, 1, 0, 0, 0, 0, 0, 0, 1, 2, 3
    };
    static const short widths[BUTTON_SIZE] = {
        6, 8, 10, 12, 12, 12, 12, 12, 12, 10, 8, 6
    };
    XRectangle rows[BUTTON_SIZE];
    for (short y = 0; y < BUTTON_SIZE; y++) {
        rows[y].x = left_edges[y];
        rows[y].y = y;
        rows[y].width = widths[y];
        rows[y].height = 1;
    }
    XShapeCombineRectangles(wm->display, button, ShapeBounding, 0, 0,
                            rows, BUTTON_SIZE, ShapeSet, Unsorted);
}

static void apply_frame_shape(MacosWm *wm, ManagedWindow *managed) {
    if (!wm->shape_extension_available) return;
    if (!wm->rounded_corners || managed->fullscreen || managed->maximized) {
        XShapeCombineMask(wm->display, managed->frame, ShapeBounding, 0, 0,
                          None, ShapeSet);
        return;
    }

    int width = managed->width;
    int height = managed->height + titlebar_height(managed);
    if (width < 1 || height < 1) return;
    XRectangle *rows = g_new0(XRectangle, (gsize)height);
    int radius = MIN(16, MIN(width, height) / 2);
    for (int y = 0; y < height; y++) {
        int inset = 0;
        if (y < radius) {
            double dy = (double)radius - y - 0.5;
            inset = MAX(0, radius - (int)sqrt(MAX(0.0,
                (double)radius * radius - dy * dy)));
        } else if (y >= height - radius) {
            double dy = (double)y - (height - radius) + 0.5;
            inset = MAX(0, radius - (int)sqrt(MAX(0.0,
                (double)radius * radius - dy * dy)));
        }
        rows[y].x = (short)MIN(inset, SHRT_MAX);
        rows[y].y = (short)MIN(y, SHRT_MAX);
        rows[y].width = (unsigned short)MAX(1, MIN(width - 2 * inset, USHRT_MAX));
        rows[y].height = 1;
    }
    XShapeCombineRectangles(wm->display, managed->frame, ShapeBounding, 0, 0,
                            rows, height, ShapeSet, Unsorted);
    g_free(rows);
}

static char *window_manager_settings_path(void) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home && *config_home) {
        return g_build_filename(config_home, "macos-de", "settings.ini", NULL);
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    return g_build_filename(home, ".config", "macos-de", "settings.ini", NULL);
}

static void reload_window_manager_preferences(MacosWm *wm) {
    char *path = window_manager_settings_path();
    if (!path) return;

    struct stat info;
    if (stat(path, &info) != 0) {
        g_free(path);
        return;
    }
    if (wm->settings_mtime_valid &&
        wm->settings_mtime.tv_sec == info.st_mtim.tv_sec &&
        wm->settings_mtime.tv_nsec == info.st_mtim.tv_nsec) {
        g_free(path);
        return;
    }

    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, &error)) {
        fprintf(stderr, "macos-wm: could not load appearance preferences from %s: %s\n",
                path, error->message);
        g_clear_error(&error);
        g_key_file_unref(settings);
        g_free(path);
        return;
    }

    char *buttons = g_key_file_get_string(
        settings, "WindowManager", "buttons", NULL);
    char *corners = g_key_file_get_string(
        settings, "WindowManager", "corners", NULL);
    int circular_buttons = buttons ? strcmp(buttons, "boxes") != 0 : 1;
    int rounded_corners = corners ? strcmp(corners, "sharp") != 0 : 1;
    gboolean changed = circular_buttons != wm->circular_buttons ||
                       rounded_corners != wm->rounded_corners;
    wm->circular_buttons = circular_buttons;
    wm->rounded_corners = rounded_corners;
    wm->settings_mtime = info.st_mtim;
    wm->settings_mtime_valid = 1;

    if (changed) {
        for (ManagedWindow *managed = wm->windows; managed;
             managed = managed->next) {
            apply_frame_shape(wm, managed);
            update_resize_handles(wm, managed);
            for (int i = 0; i < 3; i++)
                apply_button_shape(wm, managed->buttons[i]);
            draw_titlebar(wm, managed);
        }
        XFlush(wm->display);
    }

    g_free(buttons);
    g_free(corners);
    g_key_file_unref(settings);
    g_free(path);
}

static ManagedWindow *find_client(MacosWm *wm, Window window) {
    for (ManagedWindow *managed = wm->windows; managed; managed = managed->next) {
        if (managed->client == window || managed->frame == window ||
            managed->titlebar == window) return managed;
        for (int i = 0; i < 3; i++) {
            if (managed->buttons[i] == window) return managed;
        }
        for (int i = 0; i < 8; i++) {
            if (managed->resize_handles[i] == window) return managed;
        }
    }
    return NULL;
}

static int window_has_atom(MacosWm *wm, Window window, Atom property, Atom value) {
    Atom actual_type;
    int actual_format;
    unsigned long count, bytes_after;
    unsigned char *data = NULL;
    int found = 0;

    if (XGetWindowProperty(wm->display, window, property,
                           0, 32, False, XA_ATOM, &actual_type, &actual_format,
                           &count, &bytes_after, &data) == Success &&
        data && actual_type == XA_ATOM && actual_format == 32) {
        Atom *types = (Atom *)data;
        for (unsigned long i = 0; i < count; i++) {
            if (types[i] == value) {
                found = 1;
                break;
            }
        }
    }
    if (data) XFree(data);
    return found;
}

static int window_has_type(MacosWm *wm, Window window, Atom type) {
    return window_has_atom(wm, window, wm->atoms.net_wm_window_type, type);
}

static int should_decorate(MacosWm *wm, Window window) {
    WmAtoms *a = &wm->atoms;
    return !window_has_type(wm, window, a->net_wm_window_type_desktop) &&
           !window_has_type(wm, window, a->net_wm_window_type_dock) &&
           !window_has_type(wm, window, a->net_wm_window_type_splash) &&
           !window_has_type(wm, window, a->net_wm_window_type_menu) &&
           !window_has_type(wm, window, a->net_wm_window_type_dropdown_menu) &&
           !window_has_type(wm, window, a->net_wm_window_type_popup_menu) &&
           !window_has_type(wm, window, a->net_wm_window_type_tooltip) &&
           !window_has_type(wm, window, a->net_wm_window_type_notification) &&
           !window_has_type(wm, window, a->net_wm_window_type_dnd);
}

static int top_panel_height(MacosWm *wm) {
    Window root_return, parent_return, *children = NULL;
    unsigned int child_count = 0;
    int panel_height = 0;
    int screen_width = DisplayWidth(wm->display, wm->screen);

    if (!XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                    &children, &child_count)) {
        return 0;
    }

    for (unsigned int i = 0; i < child_count; i++) {
        if (!window_has_type(wm, children[i], wm->atoms.net_wm_window_type_dock)) {
            continue;
        }
        XWindowAttributes attributes;
        if (!XGetWindowAttributes(wm->display, children[i], &attributes) ||
            attributes.map_state == IsUnmapped) continue;

        Atom actual_type;
        int actual_format;
        unsigned long count, bytes_after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(wm->display, children[i],
                               wm->atoms.net_wm_strut_partial, 0, 12, False,
                               XA_CARDINAL, &actual_type, &actual_format,
                               &count, &bytes_after, &data) == Success &&
            data && actual_type == XA_CARDINAL && actual_format == 32 &&
            count >= 12) {
            unsigned long *strut = (unsigned long *)data;
            int spans_screen = strut[8] < (unsigned long)screen_width &&
                               strut[8] <= strut[9];
            if (strut[2] > 0 && spans_screen && (int)strut[2] > panel_height) {
                panel_height = (int)strut[2];
            }
        }
        if (data) XFree(data);
    }

    if (children) XFree(children);
    return panel_height;
}

static int window_has_panel_strut(MacosWm *wm, Window window) {
    Atom actual_type;
    int actual_format;
    unsigned long count, bytes_after;
    unsigned char *data = NULL;
    int has_strut =
        XGetWindowProperty(wm->display, window, wm->atoms.net_wm_strut_partial,
                           0, 12, False, XA_CARDINAL, &actual_type,
                           &actual_format, &count, &bytes_after, &data) == Success &&
        data && actual_type == XA_CARDINAL && actual_format == 32 && count >= 12;
    if (data) XFree(data);
    return has_strut;
}

static int overlaps_dock(MacosWm *wm, ManagedWindow *managed) {
    if (!managed || managed->fullscreen || managed->maximized) return 0;
    Window root_return, parent_return, *children = NULL;
    unsigned int child_count = 0;
    int overlaps = 0;
    if (!XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                    &children, &child_count)) return 0;

    int frame_bottom = managed->y + managed->height +
                       titlebar_height(managed);
    int frame_right = managed->x + managed->width;
    for (unsigned int i = 0; i < child_count; i++) {
        if (!window_has_type(wm, children[i], wm->atoms.net_wm_window_type_dock) ||
            window_has_panel_strut(wm, children[i])) continue;
        XWindowAttributes attributes;
        if (!XGetWindowAttributes(wm->display, children[i], &attributes)) continue;
        int right = attributes.x + attributes.width;
        int bottom = attributes.y + attributes.height;
        if (managed->x < right && frame_right > attributes.x &&
            managed->y < bottom && frame_bottom > attributes.y) {
            overlaps = 1;
            break;
        }
    }
    if (children) XFree(children);
    return overlaps;
}

static void update_shell_visibility(MacosWm *wm) {
    int fullscreen = wm->active && wm->active->fullscreen;
    int hide_dock = fullscreen ||
                    (wm->active && (wm->active->maximized ||
                                    overlaps_dock(wm, wm->active))) ||
                    (wm->drag_window && overlaps_dock(wm, wm->drag_window));
    if (fullscreen == wm->panel_hidden && hide_dock == wm->dock_hidden) return;

    Window root_return, parent_return, *children = NULL;
    unsigned int child_count = 0;
    if (!XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                    &children, &child_count)) return;

    for (unsigned int i = 0; i < child_count; i++) {
        if (!window_has_type(wm, children[i], wm->atoms.net_wm_window_type_dock)) {
            continue;
        }
        int hide = fullscreen ||
                   (!window_has_panel_strut(wm, children[i]) && hide_dock);
        XWindowAttributes attributes;
        if (!XGetWindowAttributes(wm->display, children[i], &attributes)) continue;
        if (hide && attributes.map_state != IsUnmapped) {
            XUnmapWindow(wm->display, children[i]);
        } else if (!hide && attributes.map_state == IsUnmapped) {
            XMapWindow(wm->display, children[i]);
        }
    }
    if (children) XFree(children);
    wm->panel_hidden = fullscreen;
    wm->dock_hidden = hide_dock;
}

static void restack_dock_windows(MacosWm *wm) {
    Window root_return, parent_return, *children = NULL;
    unsigned int child_count = 0;
    if (!XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                    &children, &child_count)) {
        return;
    }

    for (unsigned int i = 0; i < child_count; i++) {
        XWindowAttributes attributes;
        if (XGetWindowAttributes(wm->display, children[i], &attributes) &&
            attributes.map_state != IsUnmapped &&
            window_has_type(wm, children[i], wm->atoms.net_wm_window_type_dock)) {
            XRaiseWindow(wm->display, children[i]);
        }
    }
    if (children) XFree(children);
}

static char *window_title(MacosWm *wm, Window client) {
    Atom actual_type;
    int actual_format;
    unsigned long count, bytes_after;
    unsigned char *value = NULL;
    if (XGetWindowProperty(wm->display, client, wm->atoms.net_wm_name,
                           0, 256, False, AnyPropertyType, &actual_type,
                           &actual_format, &count, &bytes_after, &value) == Success &&
        value) {
        char *title = strndup((char *)value, count);
        XFree(value);
        return title;
    }

    XTextProperty property;
    if (XGetWMName(wm->display, client, &property) && property.value) {
        char *title = strndup((char *)property.value, property.nitems);
        XFree(property.value);
        return title;
    }
    return strdup("Window");
}

static void draw_titlebar(MacosWm *wm, ManagedWindow *managed) {
    if (!managed->titlebar || titlebar_height(managed) == 0) return;
    Display *display = wm->display;
    XSetWindowBackground(display, managed->titlebar, wm->title_color);
    XClearWindow(display, managed->titlebar);

    char *title = window_title(wm, managed->client);
    int title_width;
    if (wm->xft_font) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(display, wm->xft_font, (const FcChar8 *)title,
                           (int)strlen(title), &extents);
        title_width = (int)extents.xOff;
    } else {
        title_width = XTextWidth(wm->font, title, (int)strlen(title));
    }
    int available_width = managed->width - 90;
    int title_x = 68 + (available_width - title_width) / 2;
    if (title_x < 68) title_x = 68;
    if (wm->xft_font) {
        XftDraw *draw = XftDrawCreate(
            display, managed->titlebar, DefaultVisual(display, wm->screen),
            DefaultColormap(display, wm->screen));
        if (draw) {
            XftDrawStringUtf8(draw, &wm->xft_text_color, wm->xft_font,
                              title_x, 20, (const FcChar8 *)title,
                              (int)strlen(title));
            XftDrawDestroy(draw);
        }
    } else {
        XSetForeground(display, wm->gc, wm->text_color);
        XDrawString(display, managed->titlebar, wm->gc, title_x, 20,
                    title, (int)strlen(title));
    }
    free(title);

    for (int i = 0; i < 3; i++) {
        XSetWindowBackground(display, managed->buttons[i], wm->title_color);
        XClearWindow(display, managed->buttons[i]);
        XSetForeground(display, wm->gc, wm->button_colors[i]);
        if (wm->circular_buttons) {
            XFillArc(display, managed->buttons[i], wm->gc, 0, 0,
                     BUTTON_SIZE, BUTTON_SIZE, 0, 360 * 64);
        } else {
            XFillRectangle(display, managed->buttons[i], wm->gc, 0, 0,
                           BUTTON_SIZE, BUTTON_SIZE);
        }
        XSetForeground(display, wm->gc, wm->button_hover_colors[i]);
        if (wm->circular_buttons) {
            XDrawArc(display, managed->buttons[i], wm->gc, 0, 0,
                     BUTTON_SIZE - 1, BUTTON_SIZE - 1, 0, 360 * 64);
        } else {
            XDrawRectangle(display, managed->buttons[i], wm->gc, 0, 0,
                           BUTTON_SIZE - 1, BUTTON_SIZE - 1);
        }
    }
}

static void publish_client_list(MacosWm *wm) {
    int count = 0;
    for (ManagedWindow *it = wm->windows; it; it = it->next) count++;
    Window *clients = count ? calloc((size_t)count, sizeof(Window)) : NULL;
    int index = 0;
    for (ManagedWindow *it = wm->windows; it; it = it->next) {
        clients[index++] = it->client;
    }
    XChangeProperty(wm->display, wm->root, wm->atoms.net_client_list,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)clients, count);
    free(clients);
}

static void set_wm_state(MacosWm *wm, Window client, long state) {
    long data[2] = { state, None };
    XChangeProperty(wm->display, client, wm->atoms.wm_state,
                    wm->atoms.wm_state, 32, PropModeReplace,
                    (unsigned char *)data, 2);
}

static void set_active_window(MacosWm *wm, ManagedWindow *managed) {
    if (!managed || managed->minimized) return;
    wm->active = managed;
    XChangeProperty(wm->display, wm->root, wm->atoms.net_active_window,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&managed->client, 1);
    XSetInputFocus(wm->display, managed->client, RevertToPointerRoot, CurrentTime);

    XWMHints *hints = XGetWMHints(wm->display, managed->client);
    if (!hints || (hints->flags & InputHint && hints->input)) {
        XClientMessageEvent take_focus = {0};
        take_focus.type = ClientMessage;
        take_focus.window = managed->client;
        take_focus.message_type = wm->atoms.wm_protocols;
        take_focus.format = 32;
        take_focus.data.l[0] = (long)intern_atom(wm->display, "WM_TAKE_FOCUS");
        take_focus.data.l[1] = CurrentTime;
        XSendEvent(wm->display, managed->client, False, NoEventMask,
                   (XEvent *)&take_focus);
    }
    if (hints) XFree(hints);
    update_shell_visibility(wm);
    XRaiseWindow(wm->display, managed->frame);
    if (!managed->fullscreen) restack_dock_windows(wm);
}

static void send_configure_notify(MacosWm *wm, ManagedWindow *managed) {
    XEvent event = {0};
    event.xconfigure.type = ConfigureNotify;
    event.xconfigure.display = wm->display;
    event.xconfigure.event = managed->client;
    event.xconfigure.window = managed->client;
    event.xconfigure.x = managed->x;
    event.xconfigure.y = managed->y;
    event.xconfigure.width = managed->width;
    event.xconfigure.height = managed->height;
    event.xconfigure.border_width = managed->border_width;
    event.xconfigure.above = None;
    event.xconfigure.override_redirect = False;
    XSendEvent(wm->display, managed->client, False,
               StructureNotifyMask, &event);
}

static void update_frame_geometry(MacosWm *wm, ManagedWindow *managed) {
    int decoration = titlebar_height(managed);
    XMoveResizeWindow(wm->display, managed->frame,
                      managed->x, managed->y,
                      (unsigned int)managed->width,
                      (unsigned int)(managed->height + decoration));
    apply_frame_shape(wm, managed);
    XMoveResizeWindow(wm->display, managed->client, 0, decoration,
                      (unsigned int)managed->width,
                      (unsigned int)managed->height);
    update_resize_handles(wm, managed);
    if (managed->titlebar && decoration) XMapWindow(wm->display, managed->titlebar);
    else if (managed->titlebar) XUnmapWindow(wm->display, managed->titlebar);
    send_configure_notify(wm, managed);
}

static void update_net_wm_state(MacosWm *wm, ManagedWindow *managed) {
    Atom states[3];
    int count = 0;
    if (managed->maximized) {
        states[count++] = wm->atoms.net_wm_state_maximized_horz;
        states[count++] = wm->atoms.net_wm_state_maximized_vert;
    }
    if (managed->fullscreen) states[count++] = wm->atoms.net_wm_state_fullscreen;
    XChangeProperty(wm->display, managed->client, wm->atoms.net_wm_state,
                    XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, count);
}

static void toggle_maximize(MacosWm *wm, ManagedWindow *managed) {
    if (!managed->maximized) {
        managed->restore_x = managed->x;
        managed->restore_y = managed->y;
        managed->restore_width = managed->width;
        managed->restore_height = managed->height;
        managed->x = 0;
        managed->y = top_panel_height(wm);
        managed->width = DisplayWidth(wm->display, wm->screen);
        managed->height = DisplayHeight(wm->display, wm->screen) -
                          managed->y - titlebar_height(managed);
        managed->maximized = 1;
    } else {
        managed->x = managed->restore_x;
        managed->y = managed->restore_y;
        managed->width = managed->restore_width;
        managed->height = managed->restore_height;
        managed->maximized = 0;
    }
    update_net_wm_state(wm, managed);
    update_frame_geometry(wm, managed);
    draw_titlebar(wm, managed);
    update_shell_visibility(wm);
    update_shell_visibility(wm);
}

static void toggle_fullscreen(MacosWm *wm, ManagedWindow *managed) {
    if (!managed->fullscreen) {
        managed->restore_x = managed->x;
        managed->restore_y = managed->y;
        managed->restore_width = managed->width;
        managed->restore_height = managed->height;
        managed->fullscreen = 1;
        managed->maximized = 0;
        managed->x = 0;
        managed->y = 0;
        managed->width = DisplayWidth(wm->display, wm->screen);
        managed->height = DisplayHeight(wm->display, wm->screen);
    } else {
        managed->fullscreen = 0;
        managed->x = managed->restore_x;
        managed->y = managed->restore_y;
        managed->width = managed->restore_width;
        managed->height = managed->restore_height;
    }
    update_net_wm_state(wm, managed);
    update_frame_geometry(wm, managed);
    update_shell_visibility(wm);
    if (managed->fullscreen) XRaiseWindow(wm->display, managed->frame);
    else restack_dock_windows(wm);
}

static void set_state_action(MacosWm *wm, ManagedWindow *managed,
                             Atom state, long action) {
    int enabled = 0;
    if (state == wm->atoms.net_wm_state_maximized_horz ||
        state == wm->atoms.net_wm_state_maximized_vert) {
        enabled = managed->maximized;
        if (action == 0) enabled = 0;
        else if (action == 1) enabled = 1;
        else enabled = !enabled;
        if (enabled != managed->maximized) toggle_maximize(wm, managed);
    } else if (state == wm->atoms.net_wm_state_fullscreen) {
        enabled = managed->fullscreen;
        if (action == 0) enabled = 0;
        else if (action == 1) enabled = 1;
        else enabled = !enabled;
        if (enabled != managed->fullscreen) toggle_fullscreen(wm, managed);
    }
}

static int window_has_initial_state(MacosWm *wm, Window window, Atom state) {
    return window_has_atom(wm, window, wm->atoms.net_wm_state, state);
}

static void request_close(MacosWm *wm, ManagedWindow *managed) {
    Atom *protocols = NULL;
    int count = 0;
    if (XGetWMProtocols(wm->display, managed->client, &protocols, &count)) {
        for (int i = 0; i < count; i++) {
            if (protocols[i] == wm->atoms.wm_delete_window) {
                XEvent event = {0};
                event.xclient.type = ClientMessage;
                event.xclient.window = managed->client;
                event.xclient.message_type = wm->atoms.wm_protocols;
                event.xclient.format = 32;
                event.xclient.data.l[0] = (long)wm->atoms.wm_delete_window;
                event.xclient.data.l[1] = CurrentTime;
                XSendEvent(wm->display, managed->client, False, NoEventMask, &event);
                XFree(protocols);
                return;
            }
        }
        if (protocols) XFree(protocols);
    }
    XKillClient(wm->display, managed->client);
}

static void minimize_window(MacosWm *wm, ManagedWindow *managed) {
    if (managed->minimized) return;
    managed->minimized = 1;
    set_wm_state(wm, managed->client, IconicState);
    XUnmapWindow(wm->display, managed->frame);
    if (wm->active == managed) {
        wm->active = NULL;
        XDeleteProperty(wm->display, wm->root, wm->atoms.net_active_window);
        for (ManagedWindow *it = wm->windows; it; it = it->next) {
            if (!it->minimized && it != managed) {
                set_active_window(wm, it);
                break;
            }
        }
    }
    update_shell_visibility(wm);
}

static void restore_window(MacosWm *wm, ManagedWindow *managed) {
    if (managed->minimized) {
        managed->minimized = 0;
        set_wm_state(wm, managed->client, NormalState);
        XMapWindow(wm->display, managed->frame);
        XMapWindow(wm->display, managed->client);
    }
    set_active_window(wm, managed);
}

static void unmanage_window(MacosWm *wm, ManagedWindow *managed, int destroyed) {
    if (wm->switcher_selection == managed) wm->switcher_selection = NULL;
    ManagedWindow **link = &wm->windows;
    while (*link && *link != managed) link = &(*link)->next;
    if (*link) *link = managed->next;

    if (!destroyed) {
        XUnmapWindow(wm->display, managed->frame);
        XReparentWindow(wm->display, managed->client, wm->root,
                        managed->x, managed->y);
        XRemoveFromSaveSet(wm->display, managed->client);
        XSetWindowBorderWidth(wm->display, managed->client,
                              (unsigned int)managed->border_width);
    }
    if (managed->frame) XDestroyWindow(wm->display, managed->frame);
    if (!destroyed) set_wm_state(wm, managed->client, WithdrawnState);
    int was_active = wm->active == managed;
    if (was_active) {
        wm->active = NULL;
        XDeleteProperty(wm->display, wm->root, wm->atoms.net_active_window);
    }
    free(managed);
    publish_client_list(wm);
    if (was_active) {
        for (ManagedWindow *it = wm->windows; it; it = it->next) {
            if (!it->minimized) {
                set_active_window(wm, it);
                break;
            }
        }
    }
    update_shell_visibility(wm);
}

static ManagedWindow *next_window(MacosWm *wm, ManagedWindow *current,
                                  int backwards) {
    ManagedWindow *candidate = NULL;
    if (!wm->windows) return NULL;

    if (backwards) {
        ManagedWindow *previous = NULL;
        ManagedWindow *it = wm->windows;
        while (it && it != current) {
            previous = it;
            it = it->next;
        }
        if (previous) candidate = previous;
        else {
            for (it = wm->windows; it; it = it->next) {
                candidate = it;
            }
        }
    } else {
        ManagedWindow *it = current ? current->next : wm->windows;
        for (; it; it = it->next) {
            candidate = it;
            break;
        }
        if (!candidate) {
            for (it = wm->windows; it && it != current; it = it->next) {
                candidate = it;
            }
        }
    }
    return candidate;
}

static void draw_switcher(MacosWm *wm) {
    if (!wm->switcher_window) return;
    int rows = 0;
    for (ManagedWindow *it = wm->windows; it; it = it->next) rows++;
    int width = 420;
    int row_height = 34;
    int height = rows * row_height + 20;
    int max_height = DisplayHeight(wm->display, wm->screen) - 80;
    if (height > max_height) height = max_height;
    XMoveResizeWindow(wm->display, wm->switcher_window,
                      (DisplayWidth(wm->display, wm->screen) - width) / 2,
                      (DisplayHeight(wm->display, wm->screen) - height) / 2,
                      (unsigned int)width, (unsigned int)height);
    XMapRaised(wm->display, wm->switcher_window);
    XSetWindowBackground(wm->display, wm->switcher_window,
                         named_color(wm->display, "#292a30", BlackPixel(wm->display, wm->screen)));
    XClearWindow(wm->display, wm->switcher_window);

    int index = 0;
    for (ManagedWindow *it = wm->windows; it; it = it->next, index++) {
        int y = 10 + index * row_height;
        if (y + row_height > height) break;
        if (it == wm->switcher_selection) {
            XSetForeground(wm->display, wm->gc,
                           named_color(wm->display, "#596579", wm->border_color));
            XFillRectangle(wm->display, wm->switcher_window, wm->gc,
                           8, y, (unsigned int)(width - 16),
                           (unsigned int)(row_height - 2));
        }
        char *title = window_title(wm, it->client);
        XSetForeground(wm->display, wm->gc, WhitePixel(wm->display, wm->screen));
        XDrawString(wm->display, wm->switcher_window, wm->gc, 20, y + 22,
                    title, (int)strlen(title));
        free(title);
    }
    XFlush(wm->display);
}

static void switcher_step(MacosWm *wm, int backwards) {
    wm->switcher_selection = next_window(wm, wm->active, backwards);
    if (!wm->switcher_selection) return;

    if (!wm->switcher_window) {
        XSetWindowAttributes attributes = {0};
        attributes.override_redirect = True;
        attributes.background_pixel = named_color(
            wm->display, "#292a30", BlackPixel(wm->display, wm->screen));
        wm->switcher_window = XCreateWindow(
            wm->display, wm->root, 0, 0, 420, 100, 0,
            CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWBackPixel, &attributes);
    }
    wm->switcher_visible = 1;
    draw_switcher(wm);
    restore_window(wm, wm->switcher_selection);
    draw_switcher(wm);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wm->switcher_hide_at_ms =
        (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 650;
}

static void handle_key_press(MacosWm *wm, XKeyEvent *event) {
    KeySym key = XLookupKeysym(event, 0);
    KeySym volume_key = is_volume_keysym(key)
        ? key : volume_keysym_for_keycode(wm->display, event->keycode);
    KeySym brightness_key = is_brightness_keysym(key)
        ? key : brightness_keysym_for_keycode(wm->display, event->keycode);
    if (is_brightness_keysym(brightness_key)) {
        run_brightness_command(brightness_key);
    } else if (is_volume_keysym(volume_key)) {
        run_volume_command(wm, volume_key);
    } else if ((event->state & (ControlMask | Mod1Mask)) ==
                   (ControlMask | Mod1Mask) &&
               (key == XK_Up || key == XK_Down || key == XK_m ||
                key == XK_M)) {
        run_volume_command(wm, key == XK_Up ? XF86XK_AudioRaiseVolume :
                               key == XK_Down ? XF86XK_AudioLowerVolume :
                                                XF86XK_AudioMute);
    } else if ((event->state & Mod1Mask) && key == XK_Tab) {
        switcher_step(wm, (event->state & ShiftMask) != 0);
    } else if ((event->state & Mod1Mask) && key == XK_F4 && wm->active) {
        request_close(wm, wm->active);
    } else if (key == XK_F11 && wm->active) {
        toggle_fullscreen(wm, wm->active);
    } else if (key == XK_Print) {
        run_screenshot_manager((event->state & ShiftMask) ? "--select" : "--capture");
    }
}

static void reap_child_processes(int signal_number) {
    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static ManagedWindow *manage_window(MacosWm *wm, Window client) {
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(wm->display, client, &attributes) ||
        attributes.override_redirect || find_client(wm, client)) return NULL;

    if (!should_decorate(wm, client)) {
        XMapWindow(wm->display, client);
        if (window_has_type(wm, client, wm->atoms.net_wm_window_type_desktop)) {
            XLowerWindow(wm->display, client);
        } else {
            XRaiseWindow(wm->display, client);
        }
        return NULL;
    }

    ManagedWindow *managed = calloc(1, sizeof(*managed));
    if (!managed) return NULL;
    managed->client = client;
    managed->client_decorated =
        macos_client_has_own_decorations(wm->display, client);
    managed->x = attributes.x;
    managed->y = attributes.y;
    managed->width = attributes.width > 0 ? attributes.width : 1;
    managed->height = attributes.height > 0 ? attributes.height : 1;
    managed->border_width = attributes.border_width;
    if (window_has_initial_state(wm, client,
                                 wm->atoms.net_wm_state_fullscreen)) {
        managed->fullscreen = 1;
        managed->restore_x = managed->x;
        managed->restore_y = managed->y;
        managed->restore_width = managed->width;
        managed->restore_height = managed->height;
        managed->x = 0;
        managed->y = 0;
        managed->width = DisplayWidth(wm->display, wm->screen);
        managed->height = DisplayHeight(wm->display, wm->screen);
    } else if (window_has_initial_state(
                   wm, client, wm->atoms.net_wm_state_maximized_horz) &&
               window_has_initial_state(
                   wm, client, wm->atoms.net_wm_state_maximized_vert)) {
        managed->maximized = 1;
        managed->restore_x = managed->x;
        managed->restore_y = managed->y;
        managed->restore_width = managed->width;
        managed->restore_height = managed->height;
        managed->x = 0;
        managed->y = top_panel_height(wm);
        managed->width = DisplayWidth(wm->display, wm->screen);
        managed->height = DisplayHeight(wm->display, wm->screen) -
                          managed->y - titlebar_height(managed);
    } else {
        int panel_height = top_panel_height(wm);
        if (managed->y < panel_height) managed->y = panel_height;
    }

    int decoration = titlebar_height(managed);
    managed->frame = XCreateSimpleWindow(
        wm->display, wm->root, managed->x, managed->y,
        (unsigned int)managed->width,
        (unsigned int)(managed->height + decoration),
        FRAME_BORDER, wm->border_color, WhitePixel(wm->display, wm->screen));
    XSelectInput(wm->display, managed->frame,
                 SubstructureNotifyMask | ExposureMask | ButtonPressMask |
                 PointerMotionMask | EnterWindowMask | LeaveWindowMask);
    for (int i = 0; i < 8; i++) {
        XSetWindowAttributes handle_attributes = {0};
        handle_attributes.event_mask = ButtonPressMask | PointerMotionMask |
                                      EnterWindowMask | LeaveWindowMask;
        managed->resize_handles[i] = XCreateWindow(
            wm->display, managed->frame, 0, 0, 1, 1, 0, 0, InputOnly,
            CopyFromParent, CWEventMask, &handle_attributes);
        Cursor cursor = cursor_for_resize_edges(wm,
                                                 resize_handle_edges(i));
        if (cursor != None) {
            XDefineCursor(wm->display, managed->resize_handles[i], cursor);
        }
    }

    if (decoration) {
        managed->titlebar = XCreateSimpleWindow(
            wm->display, managed->frame, 0, 0,
            (unsigned int)managed->width, TITLEBAR_HEIGHT,
            0, wm->title_color, wm->title_color);
        XSelectInput(wm->display, managed->titlebar,
                     ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask);
        if (wm->move_cursor != None) {
            XDefineCursor(wm->display, managed->titlebar, wm->move_cursor);
        }

        for (int i = 0; i < 3; i++) {
            int x = BUTTON_LEFT + i * (BUTTON_SIZE + BUTTON_GAP);
            managed->buttons[i] = XCreateSimpleWindow(
                wm->display, managed->titlebar, x, BUTTON_TOP,
                BUTTON_SIZE, BUTTON_SIZE, 0,
                wm->title_color, wm->title_color);
            apply_button_shape(wm, managed->buttons[i]);
            XSelectInput(wm->display, managed->buttons[i],
                         ExposureMask | ButtonPressMask | EnterWindowMask |
                         LeaveWindowMask);
        }
    }

    XSelectInput(wm->display, client,
                 PropertyChangeMask | StructureNotifyMask | EnterWindowMask |
                 LeaveWindowMask | PointerMotionMask);
    XAddToSaveSet(wm->display, client);
    XSetWindowBorderWidth(wm->display, client, 0);
    XReparentWindow(wm->display, client, managed->frame, 0, decoration);
    XResizeWindow(wm->display, client, (unsigned int)managed->width,
                  (unsigned int)managed->height);
    XGrabButton(wm->display, Button3, Mod1Mask, managed->frame, False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(wm->display, Button1, Mod1Mask, managed->frame, False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    if (wm->normal_cursor != None) {
        XDefineCursor(wm->display, managed->frame, wm->normal_cursor);
    }

    managed->next = wm->windows;
    wm->windows = managed;
    draw_titlebar(wm, managed);
    XMapWindow(wm->display, managed->client);
    if (decoration) {
        apply_frame_shape(wm, managed);
        XMapWindow(wm->display, managed->titlebar);
        XMapSubwindows(wm->display, managed->titlebar);
    }
    XMapWindow(wm->display, managed->frame);
    update_resize_handles(wm, managed);
    set_wm_state(wm, client, NormalState);
    update_net_wm_state(wm, managed);
    publish_client_list(wm);
    send_configure_notify(wm, managed);
    set_active_window(wm, managed);
    return managed;
}

static void draw_button_hover(MacosWm *wm, ManagedWindow *managed,
                              Window button, int entering) {
    for (int i = 0; i < 3; i++) {
        if (managed->buttons[i] == button) {
            XSetWindowBackground(wm->display, button,
                wm->title_color);
            XClearWindow(wm->display, button);
            XSetForeground(wm->display, wm->gc,
                entering ? wm->button_hover_colors[i] : wm->button_colors[i]);
            if (wm->circular_buttons) {
                XFillArc(wm->display, button, wm->gc, 0, 0,
                         BUTTON_SIZE, BUTTON_SIZE, 0, 360 * 64);
            } else {
                XFillRectangle(wm->display, button, wm->gc, 0, 0,
                               BUTTON_SIZE, BUTTON_SIZE);
            }
            XSetForeground(wm->display, wm->gc, wm->button_hover_colors[i]);
            if (wm->circular_buttons) {
                XDrawArc(wm->display, button, wm->gc, 0, 0,
                         BUTTON_SIZE - 1, BUTTON_SIZE - 1, 0, 360 * 64);
            } else {
                XDrawRectangle(wm->display, button, wm->gc, 0, 0,
                               BUTTON_SIZE - 1, BUTTON_SIZE - 1);
            }
            break;
        }
    }
}

static int resize_edges_at(const MacosWm *wm, const ManagedWindow *managed,
                           int root_x, int root_y) {
    if (managed->maximized || managed->fullscreen) return 0;
    int content_top = managed->y + titlebar_height(managed);
    int corner_margin = wm->rounded_corners
        ? ROUNDED_CORNER_RESIZE_MARGIN : SQUARE_CORNER_RESIZE_MARGIN;
    int edges = 0;
    int near_left = root_x - managed->x < corner_margin;
    int near_right = managed->x + managed->width - root_x <= corner_margin;
    int near_top = root_y - content_top < corner_margin;
    int near_bottom =
        managed->y + titlebar_height(managed) + managed->height - root_y <=
        corner_margin;
    if (near_left && near_top) edges = RESIZE_LEFT | RESIZE_TOP;
    else if (near_right && near_top) edges = RESIZE_RIGHT | RESIZE_TOP;
    else if (near_left && near_bottom) edges = RESIZE_LEFT | RESIZE_BOTTOM;
    else if (near_right && near_bottom) edges = RESIZE_RIGHT | RESIZE_BOTTOM;
    if (edges != 0)
        return edges;
    if (root_x - managed->x < RESIZE_MARGIN) edges |= RESIZE_LEFT;
    if (managed->x + managed->width - root_x <= RESIZE_MARGIN) {
        edges |= RESIZE_RIGHT;
    }
    if (root_y - content_top < RESIZE_MARGIN) edges |= RESIZE_TOP;
    if (managed->y + titlebar_height(managed) + managed->height - root_y <=
        RESIZE_MARGIN) {
        edges |= RESIZE_BOTTOM;
    }
    return edges;
}

static Cursor cursor_for_resize_edges(MacosWm *wm, int edges) {
    int horizontal = (edges & (RESIZE_LEFT | RESIZE_RIGHT)) != 0;
    int vertical = (edges & (RESIZE_TOP | RESIZE_BOTTOM)) != 0;
    if (horizontal && vertical) {
        int same_direction =
            ((edges & RESIZE_LEFT) && (edges & RESIZE_TOP)) ||
            ((edges & RESIZE_RIGHT) && (edges & RESIZE_BOTTOM));
        return wm->diagonal_cursor[same_direction ? 0 : 1];
    }
    if (horizontal) return wm->horizontal_cursor;
    if (vertical) return wm->vertical_cursor;
    return None;
}

static int resize_handle_edges(int handle) {
    static const int edges[] = {
        RESIZE_LEFT, RESIZE_RIGHT, RESIZE_TOP, RESIZE_BOTTOM,
        RESIZE_LEFT | RESIZE_TOP, RESIZE_RIGHT | RESIZE_TOP,
        RESIZE_LEFT | RESIZE_BOTTOM, RESIZE_RIGHT | RESIZE_BOTTOM
    };
    return handle >= 0 && handle < (int)(sizeof(edges) / sizeof(edges[0]))
        ? edges[handle] : 0;
}

static void update_resize_handles(MacosWm *wm, ManagedWindow *managed) {
    if (!managed->resize_handles[0]) return;
    if (managed->maximized || managed->fullscreen) {
        for (int i = 0; i < 8; i++) {
            XUnmapWindow(wm->display, managed->resize_handles[i]);
        }
        return;
    }

    int margin = MIN(RESIZE_MARGIN, MIN(managed->width, managed->height));
    int corner = wm->rounded_corners ? ROUNDED_CORNER_RESIZE_MARGIN
                                     : SQUARE_CORNER_RESIZE_MARGIN;
    corner = MIN(corner, MIN(managed->width / 2, managed->height / 2));
    corner = MAX(corner, margin);
    int decoration = titlebar_height(managed);
    int x[] = {
        0, managed->width - margin, corner, corner,
        0, managed->width - corner, 0, managed->width - corner
    };
    int y[] = {
        decoration + corner, decoration + corner, decoration,
        decoration + managed->height - margin,
        decoration, decoration, decoration + managed->height - corner,
        decoration + managed->height - corner
    };
    int width[] = {
        margin, margin, managed->width - 2 * corner,
        managed->width - 2 * corner, corner, corner, corner, corner
    };
    int height[] = {
        managed->height - 2 * corner, managed->height - 2 * corner,
        margin, margin, corner, corner, corner, corner
    };
    for (int i = 0; i < 8; i++) {
        XMoveResizeWindow(wm->display, managed->resize_handles[i], x[i], y[i],
                          (unsigned int)MAX(1, width[i]),
                          (unsigned int)MAX(1, height[i]));
        XMapWindow(wm->display, managed->resize_handles[i]);
        XRaiseWindow(wm->display, managed->resize_handles[i]);
    }
}

static void update_client_resize_cursor(MacosWm *wm,
                                        ManagedWindow *managed,
                                        int root_x, int root_y) {
    if (managed->fullscreen) return;
    int edges = resize_edges_at(wm, managed, root_x, root_y);
    Cursor cursor = cursor_for_resize_edges(wm, edges);
    if (cursor != None) XDefineCursor(wm->display, managed->client, cursor);
    else XUndefineCursor(wm->display, managed->client);
}

static void update_frame_resize_cursor(MacosWm *wm,
                                       ManagedWindow *managed,
                                       int root_x, int root_y) {
    if (managed->fullscreen) return;
    int edges = resize_edges_at(wm, managed, root_x, root_y);
    Cursor cursor = cursor_for_resize_edges(wm, edges);
    if (cursor == None) cursor = wm->normal_cursor;
    if (cursor != None) XDefineCursor(wm->display, managed->frame, cursor);
}

static void begin_window_drag(MacosWm *wm, ManagedWindow *managed,
                              XButtonEvent *event, int resizing,
                              int resize_edges) {
    wm->drag_window = managed;
    wm->resizing = resizing;
    wm->resize_edges = resize_edges;
    wm->drag_root_x = event->x_root;
    wm->drag_root_y = event->y_root;
    wm->drag_window_x = managed->x;
    wm->drag_window_y = managed->y;
    wm->drag_window_width = managed->width;
    wm->drag_window_height = managed->height;
    wm->drag_button = event->button;
    Cursor cursor = resizing ? cursor_for_resize_edges(wm, resize_edges)
                             : wm->move_cursor;
    int grab_status = XGrabPointer(
        wm->display, event->window, False,
        ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, cursor, event->time);
    if (grab_status != GrabSuccess) {
        wm->drag_pointer_grabbed = 0;
        if (cursor != None) {
            XDefineCursor(wm->display, managed->client, cursor);
            XDefineCursor(wm->display, managed->frame, cursor);
        }
    } else {
        wm->drag_pointer_grabbed = 1;
    }
}

static void update_window_drag(MacosWm *wm, int root_x, int root_y) {
    ManagedWindow *managed = wm->drag_window;
    if (!managed) return;
    int delta_x = root_x - wm->drag_root_x;
    int delta_y = root_y - wm->drag_root_y;
    if (wm->resizing) {
        int min_width = 1;
        int min_height = 1;
        XSizeHints hints;
        long supplied;
        if (XGetWMNormalHints(wm->display, managed->client, &hints, &supplied) &&
            (hints.flags & PMinSize)) {
            min_width = hints.min_width;
            min_height = hints.min_height;
        }
        managed->x = wm->drag_window_x;
        managed->y = wm->drag_window_y;
        managed->width = wm->drag_window_width;
        managed->height = wm->drag_window_height;
        if (wm->resize_edges & RESIZE_LEFT) {
            managed->x += delta_x;
            managed->width -= delta_x;
        }
        if (wm->resize_edges & RESIZE_RIGHT) managed->width += delta_x;
        if (wm->resize_edges & RESIZE_TOP) {
            managed->y += delta_y;
            managed->height -= delta_y;
        }
        if (wm->resize_edges & RESIZE_BOTTOM) managed->height += delta_y;
        if (managed->width < min_width) {
            if (wm->resize_edges & RESIZE_LEFT) {
                managed->x = wm->drag_window_x +
                             wm->drag_window_width - min_width;
            }
            managed->width = min_width;
        }
        if (managed->height < min_height) {
            if (wm->resize_edges & RESIZE_TOP) {
                managed->y = wm->drag_window_y +
                             wm->drag_window_height - min_height;
            }
            managed->height = min_height;
        }
        int panel_height = top_panel_height(wm);
        if (managed->y < panel_height) {
            int bottom = managed->y + managed->height;
            managed->y = panel_height;
            if (wm->resize_edges & RESIZE_TOP) {
                managed->height = MAX(min_height, bottom - panel_height);
            }
        }
        update_frame_geometry(wm, managed);
        draw_titlebar(wm, managed);
    } else {
        managed->x = wm->drag_window_x + delta_x;
        int requested_y = wm->drag_window_y + delta_y;
        int panel_height = top_panel_height(wm);
        managed->y = requested_y > panel_height ? requested_y : panel_height;
        XMoveWindow(wm->display, managed->frame, managed->x, managed->y);
    }
    update_shell_visibility(wm);
}

static unsigned int drag_button_mask(unsigned int button) {
    switch (button) {
    case Button1: return Button1Mask;
    case Button2: return Button2Mask;
    case Button3: return Button3Mask;
    case Button4: return Button4Mask;
    case Button5: return Button5Mask;
    default: return 0;
    }
}

static void finish_window_drag(MacosWm *wm, Time time) {
    if (!wm->drag_window) return;
    if (wm->drag_pointer_grabbed) {
        XUngrabPointer(wm->display, time);
    } else {
        XUndefineCursor(wm->display, wm->drag_window->client);
        if (wm->normal_cursor != None) {
            XDefineCursor(wm->display, wm->drag_window->frame,
                          wm->normal_cursor);
        }
    }
    wm->drag_window = NULL;
    wm->resizing = 0;
    wm->resize_edges = 0;
    wm->drag_pointer_grabbed = 0;
    wm->drag_button = 0;
    update_shell_visibility(wm);
}

static void handle_button_press(MacosWm *wm, XButtonEvent *event) {
    ManagedWindow *managed = find_client(wm, event->window);
    if (!managed) return;
    set_active_window(wm, managed);

    for (int i = 0; i < 8; i++) {
        if (event->window == managed->resize_handles[i]) {
            begin_window_drag(wm, managed, event, 1,
                              resize_handle_edges(i));
            return;
        }
    }

    if (event->window == managed->client ||
        event->window == managed->frame) {
        if ((event->state & Mod1Mask) &&
            (event->button == Button1 || event->button == Button3)) {
            begin_window_drag(wm, managed, event,
                              event->button == Button3,
                              event->button == Button3
                                  ? RESIZE_RIGHT | RESIZE_BOTTOM : 0);
            return;
        }
    }

    for (int i = 0; i < 3; i++) {
        if (event->window == managed->buttons[i]) {
            if (i == 0) request_close(wm, managed);
            else if (i == 1) minimize_window(wm, managed);
            else toggle_maximize(wm, managed);
            return;
        }
    }

    if (event->window != managed->titlebar || event->button != Button1) return;
    if (wm->last_title_click && event->time - wm->last_title_click < DOUBLE_CLICK_MS) {
        toggle_maximize(wm, managed);
        wm->last_title_click = 0;
        return;
    }
    wm->last_title_click = event->time;
    begin_window_drag(wm, managed, event, 0, 0);
}

static void handle_configure_request(MacosWm *wm, XConfigureRequestEvent *event) {
    ManagedWindow *managed = find_client(wm, event->window);
    if (!managed) {
        XWindowChanges changes = {
            .x = event->x, .y = event->y, .width = event->width,
            .height = event->height, .border_width = event->border_width,
            .sibling = event->above, .stack_mode = event->detail
        };
        XConfigureWindow(wm->display, event->window, event->value_mask, &changes);
        return;
    }

    if (managed->maximized || managed->fullscreen) return;
    if (event->value_mask & CWX) managed->x = event->x;
    if (event->value_mask & CWY) {
        int panel_height = top_panel_height(wm);
        managed->y = event->y > panel_height ? event->y : panel_height;
    }
    if (event->value_mask & CWWidth) managed->width = event->width > 0 ? event->width : 1;
    if (event->value_mask & CWHeight) managed->height = event->height > 0 ? event->height : 1;
    if (event->value_mask & CWBorderWidth) managed->border_width = event->border_width;
    update_frame_geometry(wm, managed);
    draw_titlebar(wm, managed);
}

static void handle_client_message(MacosWm *wm, XClientMessageEvent *event) {
    ManagedWindow *managed = find_client(wm, event->window);
    if (event->message_type == wm->atoms.net_wm_moveresize && managed &&
        event->format == 32) {
        int direction = (int)event->data.l[2];
        int edges = 0;
        switch (direction) {
        case 0: edges = RESIZE_TOP | RESIZE_LEFT; break;
        case 1: edges = RESIZE_TOP; break;
        case 2: edges = RESIZE_TOP | RESIZE_RIGHT; break;
        case 3: edges = RESIZE_RIGHT; break;
        case 4: edges = RESIZE_BOTTOM | RESIZE_RIGHT; break;
        case 5: edges = RESIZE_BOTTOM; break;
        case 6: edges = RESIZE_BOTTOM | RESIZE_LEFT; break;
        case 7: edges = RESIZE_LEFT; break;
        case 8:
            break;
        case 11:
            if (wm->drag_window == managed) {
                finish_window_drag(wm, CurrentTime);
            }
            return;
        default:
            return;
        }
        if (managed->fullscreen || (edges && managed->maximized)) return;
        XButtonEvent drag_event = {0};
        drag_event.type = ButtonPress;
        drag_event.display = wm->display;
        drag_event.window = managed->client;
        drag_event.root = wm->root;
        drag_event.time = CurrentTime;
        drag_event.x_root = (int)event->data.l[0];
        drag_event.y_root = (int)event->data.l[1];
        drag_event.button = event->data.l[3] > 0
            ? (unsigned int)event->data.l[3] : Button1;
        XUngrabPointer(wm->display, CurrentTime);
        begin_window_drag(wm, managed, &drag_event, edges != 0, edges);
    } else if (event->message_type == wm->atoms.net_active_window && managed) {
        restore_window(wm, managed);
    } else if (event->message_type == wm->atoms.wm_change_state &&
               event->format == 32 && event->data.l[0] == IconicState && managed) {
        minimize_window(wm, managed);
    } else if (event->message_type == wm->atoms.net_wm_state && managed &&
               event->format == 32) {
        long action = event->data.l[0];
        Atom first = (Atom)event->data.l[1];
        Atom second = (Atom)event->data.l[2];
        if (first == wm->atoms.net_wm_state_maximized_horz ||
            first == wm->atoms.net_wm_state_maximized_vert ||
            second == wm->atoms.net_wm_state_maximized_horz ||
            second == wm->atoms.net_wm_state_maximized_vert) {
            int enabled = managed->maximized;
            if (action == 0) enabled = 0;
            else if (action == 1) enabled = 1;
            else enabled = !enabled;
            if (enabled != managed->maximized) toggle_maximize(wm, managed);
        }
        if ((first == wm->atoms.net_wm_state_fullscreen ||
             second == wm->atoms.net_wm_state_fullscreen) &&
            (first != wm->atoms.net_wm_state_fullscreen ||
             second != wm->atoms.net_wm_state_fullscreen)) {
            set_state_action(wm, managed,
                first == wm->atoms.net_wm_state_fullscreen
                    ? first : second, action);
        } else if (first == wm->atoms.net_wm_state_fullscreen) {
            set_state_action(wm, managed, first, action);
        }
    }
}

static void set_supporting_wm(MacosWm *wm) {
    WmAtoms *a = &wm->atoms;
    Atom supported[] = {
        a->net_supported, a->net_supporting_wm_check, a->net_wm_name,
        a->net_wm_state, a->net_wm_state_maximized_horz,
        a->net_wm_state_maximized_vert, a->net_wm_state_fullscreen,
        a->net_active_window, a->net_client_list, a->net_wm_window_type,
        a->net_wm_moveresize,
        a->net_wm_window_type_desktop, a->net_wm_window_type_dock,
        a->net_wm_window_type_splash, a->net_wm_window_type_dialog,
        a->net_wm_window_type_menu, a->net_wm_window_type_dropdown_menu,
        a->net_wm_window_type_popup_menu, a->net_wm_window_type_tooltip,
        a->net_wm_window_type_notification, a->net_wm_window_type_dnd,
        a->net_wm_strut_partial
    };
    XChangeProperty(wm->display, wm->root, a->net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported,
                    (int)(sizeof(supported) / sizeof(supported[0])));

    wm->support_window = XCreateSimpleWindow(wm->display, wm->root,
        -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(wm->display, wm->root, a->net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&wm->support_window, 1);
    XChangeProperty(wm->display, wm->support_window, a->net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&wm->support_window, 1);
    Atom utf8_string = intern_atom(wm->display, "UTF8_STRING");
    const char wm_name[] = "macOS-DE Window Manager";
    XChangeProperty(wm->display, wm->root, a->net_wm_name, utf8_string, 8,
                    PropModeReplace, (const unsigned char *)wm_name,
                    (int)strlen(wm_name));
    XChangeProperty(wm->display, wm->support_window, a->net_wm_name, utf8_string, 8,
                    PropModeReplace, (const unsigned char *)wm_name,
                    (int)strlen(wm_name));
}

MacosWm *macos_wm_new(const char *display_name) {
    MacosWm *wm = calloc(1, sizeof(*wm));
    if (!wm) return NULL;

    wm->display = XOpenDisplay(display_name);
    if (!wm->display) {
        fprintf(stderr, "macos-wm: cannot open X display %s\n",
                display_name ? display_name : "(default)");
        free(wm);
        return NULL;
    }
    wm->screen = DefaultScreen(wm->display);
    wm->root = RootWindow(wm->display, wm->screen);
    wm->circular_buttons = 1;
    wm->rounded_corners = 1;
    int shape_event_base;
    int shape_error_base;
    wm->shape_extension_available =
        XShapeQueryExtension(wm->display, &shape_event_base,
                             &shape_error_base) != 0;
    if (!wm->shape_extension_available) {
        fprintf(stderr, "macos-wm: X Shape extension unavailable; window outlines will remain square\n");
    }
    reload_window_manager_preferences(wm);
    error_handler_wm = wm;
    XSetErrorHandler(wm_error_handler);
    initialize_atoms(wm);
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long bytes_after;
    unsigned char *volume_event = NULL;
    if (XGetWindowProperty(wm->display, wm->root,
                           wm->atoms.macos_de_volume_event, 0, 1, False,
                           XA_CARDINAL, &actual_type, &actual_format, &count,
                           &bytes_after, &volume_event) == Success &&
        volume_event && actual_type == XA_CARDINAL &&
        actual_format == 32 && count) {
        wm->volume_event_serial = ((unsigned long *)volume_event)[0];
    }
    if (volume_event) XFree(volume_event);
    setup_colors(wm);
    char *font_path = font_asset_path();
    if (font_path) {
        FcConfig *font_config = FcConfigGetCurrent();
        if (FcConfigAppFontAddFile(font_config, (const FcChar8 *)font_path)) {
            FcConfigBuildFonts(font_config);
            wm->xft_font = XftFontOpenName(
                wm->display, wm->screen,
                "SF Pro Display:style=Black Italic:size=10");
        }
        free(font_path);
    }
    if (wm->xft_font) {
        wm->xft_text_color_allocated = XftColorAllocName(
            wm->display, DefaultVisual(wm->display, wm->screen),
            DefaultColormap(wm->display, wm->screen),
            "#242428", &wm->xft_text_color);
    }
    load_wm_cursors(wm);
    struct sigaction child_action = {0};
    child_action.sa_handler = reap_child_processes;
    sigemptyset(&child_action.sa_mask);
    child_action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &child_action, NULL) != 0) {
        perror("macos-wm: could not install child reaper");
    }
    wm->font = XLoadQueryFont(wm->display, "fixed");
    if (!wm->font) wm->font = XQueryFont(wm->display, XGContextFromGC(DefaultGC(wm->display, wm->screen)));
    wm->gc = XCreateGC(wm->display, wm->root, 0, NULL);
    if (wm->font) XSetFont(wm->display, wm->gc, wm->font->fid);

    x11_error_code = 0;
    XSelectInput(wm->display, wm->root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 PropertyChangeMask | ButtonPressMask | KeyPressMask);
    XSync(wm->display, False);
    if (x11_error_code == BadAccess) {
        fprintf(stderr, "macos-wm: another window manager already owns this X display\n");
        macos_wm_free(wm);
        return NULL;
    }

    set_supporting_wm(wm);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask, wm->root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask | LockMask, wm->root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask | Mod2Mask, wm->root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask | Mod2Mask | LockMask, wm->root, False,
             GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask | ShiftMask, wm->root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask | ShiftMask | LockMask, wm->root, False,
             GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask | ShiftMask | Mod2Mask, wm->root, False,
             GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Tab),
             Mod1Mask | ShiftMask | Mod2Mask | LockMask, wm->root, False,
             GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_F4),
             AnyModifier, wm->root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_F11),
             AnyModifier, wm->root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(wm->display, XKeysymToKeycode(wm->display, XK_Print),
             AnyModifier, wm->root, False, GrabModeAsync, GrabModeAsync);
    grab_volume_keys(wm);
    grab_brightness_keys(wm);
    char selection_name[32];
    snprintf(selection_name, sizeof(selection_name), "WM_S%d", wm->screen);
    XSetSelectionOwner(wm->display, intern_atom(wm->display, selection_name),
                       wm->support_window, CurrentTime);

    Window root_return, parent_return, *children = NULL;
    unsigned int child_count = 0;
    if (XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                   &children, &child_count)) {
        for (unsigned int i = 0; i < child_count; i++) {
            XWindowAttributes attributes;
            if (XGetWindowAttributes(wm->display, children[i], &attributes) &&
                attributes.map_state != IsUnmapped && !attributes.override_redirect) {
                manage_window(wm, children[i]);
            }
        }
        if (children) XFree(children);
    }
    wm->running = 1;
    return wm;
}

int macos_wm_run(MacosWm *wm) {
    if (!wm) return 1;

    while (wm->running) {
        int pending = XPending(wm->display);
        struct pollfd connection = {
            .fd = ConnectionNumber(wm->display),
            .events = POLLIN
        };
        int timeout = !wm->drag_pointer_grabbed && wm->drag_window ? 16 :
                      wm->switcher_visible ? 50 : 250;
        int poll_status = 1;
        if (!pending) {
            poll_status = poll(&connection, 1, timeout);
            if (poll_status < 0 && errno != EINTR) {
                perror("macos-wm: poll");
                return 1;
            }
        }
        if (wm->drag_window && !wm->drag_pointer_grabbed) {
            Window root_return;
            Window child_return;
            int root_x;
            int root_y;
            int window_x;
            int window_y;
            unsigned int pointer_mask;
            unsigned int button_mask = drag_button_mask(wm->drag_button);
            if (button_mask &&
                XQueryPointer(wm->display, wm->root, &root_return,
                              &child_return, &root_x, &root_y, &window_x,
                              &window_y, &pointer_mask) &&
                (pointer_mask & button_mask)) {
                update_window_drag(wm, root_x, root_y);
            } else {
                finish_window_drag(wm, CurrentTime);
            }
            reload_window_manager_preferences(wm);
        }
        struct timespec heartbeat_time;
        clock_gettime(CLOCK_MONOTONIC, &heartbeat_time);
        long long now_ms = (long long)heartbeat_time.tv_sec * 1000 +
                           heartbeat_time.tv_nsec / 1000000;
        if (now_ms - wm->heartbeat_at_ms >= 2000) {
            const char *heartbeat_path = getenv("MACOS_WM_HEARTBEAT");
            if (heartbeat_path) {
                FILE *heartbeat = fopen(heartbeat_path, "w");
                if (heartbeat) {
                    fputs("ok\n", heartbeat);
                    fclose(heartbeat);
                }
            }
            wm->heartbeat_at_ms = now_ms;
        }
        if (wm->switcher_visible) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long now_ms = (long long)now.tv_sec * 1000 +
                               now.tv_nsec / 1000000;
            if (now_ms >= wm->switcher_hide_at_ms) {
                wm->switcher_visible = 0;
                wm->switcher_selection = NULL;
                XUnmapWindow(wm->display, wm->switcher_window);
            }
        }
        if ((!pending && poll_status <= 0) || !XPending(wm->display)) continue;
        XEvent event;
        XNextEvent(wm->display, &event);
        switch (event.type) {
        case MapRequest: {
            ManagedWindow *managed = find_client(wm, event.xmaprequest.window);
            if (managed) restore_window(wm, managed);
            else manage_window(wm, event.xmaprequest.window);
            break;
        }
        case ConfigureRequest:
            handle_configure_request(wm, &event.xconfigurerequest);
            break;
        case DestroyNotify: {
            ManagedWindow *managed = find_client(wm, event.xdestroywindow.window);
            if (managed && managed->client == event.xdestroywindow.window) {
                unmanage_window(wm, managed, 1);
            }
            break;
        }
        case UnmapNotify: {
            ManagedWindow *managed = find_client(wm, event.xunmap.window);
            if (managed && managed->client == event.xunmap.window &&
                !managed->minimized && event.xunmap.event == managed->frame) {
                unmanage_window(wm, managed, 0);
            }
            break;
        }
        case Expose: {
            ManagedWindow *managed = find_client(wm, event.xexpose.window);
            if (managed && event.xexpose.count == 0) draw_titlebar(wm, managed);
            else if (event.xexpose.window == wm->switcher_window &&
                     event.xexpose.count == 0 && wm->switcher_visible) {
                draw_switcher(wm);
            }
            break;
        }
        case PropertyNotify: {
            ManagedWindow *managed = find_client(wm, event.xproperty.window);
            if (managed && managed->client == event.xproperty.window &&
                (event.xproperty.atom == XA_WM_NAME ||
                 event.xproperty.atom == wm->atoms.net_wm_name)) {
                draw_titlebar(wm, managed);
            }
            break;
        }
        case ButtonPress:
            handle_button_press(wm, &event.xbutton);
            break;
        case ButtonRelease:
            if (wm->drag_window) {
                finish_window_drag(wm, event.xbutton.time);
            }
            break;
        case MotionNotify:
            if (!wm->drag_window) {
                ManagedWindow *managed =
                    find_client(wm, event.xmotion.window);
                int handle = -1;
                if (managed) {
                    for (int i = 0; i < 8; i++) {
                        if (managed->resize_handles[i] == event.xmotion.window) {
                            handle = i;
                            break;
                        }
                    }
                }
                if (managed && handle >= 0) {
                    Cursor cursor = cursor_for_resize_edges(
                        wm, resize_handle_edges(handle));
                    if (cursor != None) {
                        XDefineCursor(wm->display, event.xmotion.window,
                                      cursor);
                    }
                } else if (managed &&
                           managed->client == event.xmotion.window) {
                    update_client_resize_cursor(wm, managed,
                                                event.xmotion.x_root,
                                                event.xmotion.y_root);
                } else if (managed &&
                           managed->frame == event.xmotion.window) {
                    update_frame_resize_cursor(wm, managed,
                                               event.xmotion.x_root,
                                               event.xmotion.y_root);
                }
                break;
            }
            if (wm->drag_window) {
                update_window_drag(wm, event.xmotion.x_root,
                                   event.xmotion.y_root);
            }
            break;
        case EnterNotify: {
            ManagedWindow *managed = find_client(wm, event.xcrossing.window);
            if (managed && event.xcrossing.mode == NotifyNormal) {
                if (managed->client == event.xcrossing.window) {
                    set_active_window(wm, managed);
                    update_client_resize_cursor(wm, managed,
                                                event.xcrossing.x_root,
                                                event.xcrossing.y_root);
                } else if (managed->frame == event.xcrossing.window) {
                    update_frame_resize_cursor(wm, managed,
                                               event.xcrossing.x_root,
                                               event.xcrossing.y_root);
                } else if (managed->buttons[0] == event.xcrossing.window ||
                           managed->buttons[1] == event.xcrossing.window ||
                           managed->buttons[2] == event.xcrossing.window) {
                    draw_button_hover(wm, managed, event.xcrossing.window, 1);
                } else {
                    for (int i = 0; i < 8; i++) {
                        if (managed->resize_handles[i] ==
                            event.xcrossing.window) {
                            Cursor cursor = cursor_for_resize_edges(
                                wm, resize_handle_edges(i));
                            if (cursor != None) {
                                XDefineCursor(wm->display,
                                              event.xcrossing.window, cursor);
                            }
                            break;
                        }
                    }
                }
            }
            break;
        }
        case LeaveNotify: {
            ManagedWindow *managed = find_client(wm, event.xcrossing.window);
            if (managed && managed->client == event.xcrossing.window) {
                XUndefineCursor(wm->display, managed->client);
            } else if (managed) {
                draw_button_hover(wm, managed, event.xcrossing.window, 0);
            }
            break;
        }
        case ClientMessage:
            handle_client_message(wm, &event.xclient);
            break;
        case KeyPress:
            handle_key_press(wm, &event.xkey);
            break;
        }
    }
    return 0;
}

void macos_wm_free(MacosWm *wm) {
    if (!wm) return;
    if (wm->display) {
        while (wm->windows) unmanage_window(wm, wm->windows, 0);
        if (wm->switcher_window) XDestroyWindow(wm->display, wm->switcher_window);
        if (wm->normal_cursor != None) XFreeCursor(wm->display, wm->normal_cursor);
        if (wm->move_cursor != None) XFreeCursor(wm->display, wm->move_cursor);
        if (wm->horizontal_cursor != None) XFreeCursor(wm->display, wm->horizontal_cursor);
        if (wm->vertical_cursor != None) XFreeCursor(wm->display, wm->vertical_cursor);
        for (int i = 0; i < 2; i++) {
            if (wm->diagonal_cursor[i] != None) {
                XFreeCursor(wm->display, wm->diagonal_cursor[i]);
            }
        }
        for (int i = 0; i < 15; i++) {
            if (wm->working_cursors[i] != None) {
                XFreeCursor(wm->display, wm->working_cursors[i]);
            }
            if (wm->busy_cursors[i] != None) {
                XFreeCursor(wm->display, wm->busy_cursors[i]);
            }
        }
        XSelectInput(wm->display, wm->root, NoEventMask);
        if (wm->support_window) XDestroyWindow(wm->display, wm->support_window);
        if (wm->gc) XFreeGC(wm->display, wm->gc);
        if (wm->font) XFreeFont(wm->display, wm->font);
        if (wm->xft_text_color_allocated) {
            XftColorFree(wm->display, DefaultVisual(wm->display, wm->screen),
                         DefaultColormap(wm->display, wm->screen),
                         &wm->xft_text_color);
        }
        if (wm->xft_font) XftFontClose(wm->display, wm->xft_font);
        XCloseDisplay(wm->display);
    }
    error_handler_wm = NULL;
    free(wm);
}
