#include "startup.h"
#include <stdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define TOTAL_STARTUP_FRAMES 60
#define STARTUP_FRAME_INTERVAL_MS 33
#define BUFFERED_STARTUP_FRAMES 12
#define STARTUP_PREBUFFER_FRAMES 8
#define STARTUP_FADE_DURATION_US 400000
#define MAX_STARTUP_WIDTH 1280
#define MAX_STARTUP_HEIGHT 720

typedef struct {
    GtkWidget *window;
    GtkWidget *drawing_area;
    GdkPixbuf *frames[BUFFERED_STARTUP_FRAMES];
    int frame_numbers[BUFFERED_STARTUP_FRAMES];
    guint frame_head;
    guint frame_count;
    GdkPixbuf *current_pixbuf;
    GMutex frames_mutex;
    GCond frames_space_available;
    int current_frame;
    double opacity;
    gint64 fade_start_time;
    gboolean fading_out;
    gboolean loader_done;
    gboolean fade_finished;
    gboolean playback_started;
    StartupCallback callback;
    gpointer user_data;
} StartupContext;

typedef struct {
    StartupContext *context;
    char *base_path;
    int width;
    int height;
} FrameLoaderData;

static StartupContext *ctx = NULL;

static void on_startup_audio_finished(GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data) {
    GError *error = NULL;
    if (!g_subprocess_wait_check_finish(
            G_SUBPROCESS(source_object), result, &error)) {
        g_warning("Startup audio playback failed: %s",
                  error ? error->message : "unknown player error");
        g_clear_error(&error);
    }
    g_object_unref(source_object);
    (void)user_data;
}

static void play_startup_audio(const char *base_path) {
    char *audio_path = g_build_filename(base_path, "start-up.mp3", NULL);
    if (!g_file_test(audio_path, G_FILE_TEST_IS_REGULAR)) {
        g_warning("Startup audio file was not found: %s", audio_path);
        g_free(audio_path);
        return;
    }

    char *player = g_find_program_in_path("ffplay");
    const char *argv[8] = { NULL };
    if (player != NULL) {
        argv[0] = player;
        argv[1] = "-nodisp";
        argv[2] = "-autoexit";
        argv[3] = "-loglevel";
        argv[4] = "error";
        argv[5] = "-nostats";
        argv[6] = audio_path;
    } else {
        player = g_find_program_in_path("mpv");
        if (player != NULL) {
            argv[0] = player;
            argv[1] = "--no-video";
            argv[2] = "--really-quiet";
            argv[3] = audio_path;
        } else {
            player = g_find_program_in_path("mpg123");
            if (player != NULL) {
                argv[0] = player;
                argv[1] = "-q";
                argv[2] = audio_path;
            } else {
                player = g_find_program_in_path("gst-play-1.0");
                if (player != NULL) {
                    argv[0] = player;
                    argv[1] = "--no-interactive";
                    argv[2] = audio_path;
                }
            }
        }
    }

    if (player == NULL) {
        g_warning("Cannot play startup audio: install ffmpeg (ffplay), mpv, "
                  "mpg123, or GStreamer tools.");
        g_free(audio_path);
        return;
    }

    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
              G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (process == NULL) {
        g_warning("Could not start startup audio player %s: %s",
                  player, error->message);
        g_clear_error(&error);
    } else {
        g_subprocess_wait_check_async(process, NULL,
                                      on_startup_audio_finished, NULL);
    }

    g_free(player);
    g_free(audio_path);
}

static void free_startup_context(StartupContext *sc) {
    for (guint i = 0; i < BUFFERED_STARTUP_FRAMES; i++) {
        g_clear_object(&sc->frames[i]);
    }
    g_clear_object(&sc->current_pixbuf);
    g_cond_clear(&sc->frames_space_available);
    g_mutex_clear(&sc->frames_mutex);
    g_free(sc);
}

static void free_frame_loader_data(gpointer data) {
    FrameLoaderData *loader = data;
    g_free(loader->base_path);
    g_free(loader);
}

static gboolean on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    StartupContext *sc = user_data;
    if (!sc || !sc->current_pixbuf) return FALSE;

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    int win_width = gtk_widget_get_allocated_width(widget);
    int win_height = gtk_widget_get_allocated_height(widget);
    int img_width = gdk_pixbuf_get_width(sc->current_pixbuf);
    int img_height = gdk_pixbuf_get_height(sc->current_pixbuf);

    double scale = MIN((double)win_width / img_width,
                       (double)win_height / img_height);
    double draw_width = img_width * scale;
    double draw_height = img_height * scale;
    double offset_x = (win_width - draw_width) / 2.0;
    double offset_y = (win_height - draw_height) / 2.0 + win_height * 0.06;

    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, sc->current_pixbuf, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static void on_window_realize(GtkWidget *widget, gpointer user_data) {
    GdkWindow *gdk_win = gtk_widget_get_window(widget);
    if (!gdk_win) return;

    GdkCursor *blank_cursor = gdk_cursor_new_for_display(
        gdk_display_get_default(), GDK_BLANK_CURSOR);
    gdk_window_set_cursor(gdk_win, blank_cursor);
    g_object_unref(blank_cursor);
}

static void load_startup_frames(GTask *task, gpointer source_object,
                                gpointer task_data, GCancellable *cancellable) {
    FrameLoaderData *loader = task_data;
    StartupContext *sc = loader->context;
    char filepath[512];

    for (int i = 1; i < TOTAL_STARTUP_FRAMES; i++) {
        g_snprintf(filepath, sizeof(filepath), "%s/%02d.png", loader->base_path, i);
        GError *error = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
            filepath, loader->width, loader->height, TRUE, &error);

        if (!pixbuf) {
            g_task_return_new_error(
                task, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to load startup frame %s: %s", filepath,
                error ? error->message : "unknown image loading error");
            g_clear_error(&error);
            return;
        }

        g_mutex_lock(&sc->frames_mutex);
        while (sc->frame_count == BUFFERED_STARTUP_FRAMES) {
            g_cond_wait(&sc->frames_space_available, &sc->frames_mutex);
        }
        guint tail = (sc->frame_head + sc->frame_count) % BUFFERED_STARTUP_FRAMES;
        sc->frames[tail] = pixbuf;
        sc->frame_numbers[tail] = i;
        sc->frame_count++;
        g_mutex_unlock(&sc->frames_mutex);
    }

    g_task_return_boolean(task, TRUE);
}

static void on_startup_frames_loaded(GObject *source_object, GAsyncResult *result,
                                     gpointer user_data) {
    StartupContext *sc = user_data;
    GError *error = NULL;
    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        const char *message = error ? error->message :
                              "Unknown startup animation loading error.";
        g_warning("Startup animation frame loading failed: %s", message);
        sc->loader_done = TRUE;
        sc->fade_finished = TRUE;
        if (sc->window) gtk_widget_destroy(sc->window);
        if (sc->callback) sc->callback(FALSE, message, sc->user_data);
        g_clear_error(&error);
        return;
    }

    sc->loader_done = TRUE;
    if (sc->fade_finished) {
        ctx = NULL;
        free_startup_context(sc);
    }
}

static gboolean take_next_frame(StartupContext *sc, int frame, GdkPixbuf **pixbuf) {
    gboolean available = FALSE;
    g_mutex_lock(&sc->frames_mutex);
    if (sc->frame_count > 0 && sc->frame_numbers[sc->frame_head] == frame) {
        *pixbuf = sc->frames[sc->frame_head];
        sc->frames[sc->frame_head] = NULL;
        sc->frame_head = (sc->frame_head + 1) % BUFFERED_STARTUP_FRAMES;
        sc->frame_count--;
        g_cond_signal(&sc->frames_space_available);
        available = TRUE;
    }
    g_mutex_unlock(&sc->frames_mutex);
    return available;
}

static gboolean run_startup_frame(gpointer user_data) {
    StartupContext *sc = ctx;
    if (!sc) return G_SOURCE_REMOVE;

    if (sc->fade_finished) {
        if (sc->loader_done) {
            ctx = NULL;
            free_startup_context(sc);
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    if (sc->fading_out) {
        double elapsed = (double)(g_get_monotonic_time() - sc->fade_start_time);
        sc->opacity = 1.0 - elapsed / STARTUP_FADE_DURATION_US;
        if (sc->opacity <= 0.0) {
            gtk_widget_destroy(sc->window);
            sc->fade_finished = TRUE;
            if (sc->loader_done) {
                ctx = NULL;
                free_startup_context(sc);
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }
        gtk_widget_set_opacity(sc->window, sc->opacity);
        return G_SOURCE_CONTINUE;
    }

    if (!sc->playback_started) {
        g_mutex_lock(&sc->frames_mutex);
        sc->playback_started = sc->frame_count >= STARTUP_PREBUFFER_FRAMES ||
                               sc->loader_done;
        g_mutex_unlock(&sc->frames_mutex);
        if (!sc->playback_started) return G_SOURCE_CONTINUE;
    }

    if (sc->current_frame + 1 < TOTAL_STARTUP_FRAMES) {
        GdkPixbuf *next_frame = NULL;
        if (take_next_frame(sc, sc->current_frame + 1, &next_frame)) {
            sc->current_frame++;
            if (next_frame) {
                g_set_object(&sc->current_pixbuf, next_frame);
                g_object_unref(next_frame);
                gtk_widget_queue_draw(sc->drawing_area);
            }
        }
    }

    if (sc->current_frame == TOTAL_STARTUP_FRAMES - 1 && sc->loader_done) {
        sc->fading_out = TRUE;
        sc->fade_start_time = g_get_monotonic_time();
        if (sc->callback) sc->callback(TRUE, NULL, sc->user_data);
    }
    return G_SOURCE_CONTINUE;
}

void start_system_boot(const char *base_path, StartupCallback callback, gpointer user_data) {
    StartupContext *sc = g_new0(StartupContext, 1);
    ctx = sc;
    g_mutex_init(&sc->frames_mutex);
    g_cond_init(&sc->frames_space_available);
    sc->callback = callback;
    sc->user_data = user_data;
    sc->opacity = 1.0;

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);

    int target_width = MAX_STARTUP_WIDTH;
    int target_height = MAX_STARTUP_HEIGHT;
    int window_width = target_width;
    int window_height = target_height;
    GdkRectangle geometry;
    if (monitor) {
        gdk_monitor_get_geometry(monitor, &geometry);
        window_width = geometry.width;
        window_height = geometry.height;
        target_width = MAX(1, MIN(geometry.width, MAX_STARTUP_WIDTH));
        target_height = MAX(1, MIN(geometry.height, MAX_STARTUP_HEIGHT));
    }

    char filepath[512];
    g_snprintf(filepath, sizeof(filepath), "%s/00.png", base_path);
    GError *error = NULL;
    sc->frames[0] = gdk_pixbuf_new_from_file_at_scale(
        filepath, target_width, target_height, TRUE, &error);
    if (!sc->frames[0]) {
        const char *message = error ? error->message :
                              "Unknown startup frame loading error.";
        g_warning("Failed to load startup frame %s: %s", filepath, message);
        char *failure_message = g_strdup_printf(
            "Failed to load startup frame %s: %s", filepath, message);
        g_clear_error(&error);
        ctx = NULL;
        free_startup_context(sc);
        if (callback) callback(FALSE, failure_message, user_data);
        g_free(failure_message);
        return;
    }
    sc->current_pixbuf = sc->frames[0];
    g_object_ref(sc->current_pixbuf);

    sc->window = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(sc->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(sc->window), window_width, window_height);

    if (monitor) {
        gtk_window_move(GTK_WINDOW(sc->window), geometry.x, geometry.y);
    } else {
        gtk_window_set_position(GTK_WINDOW(sc->window), GTK_WIN_POS_CENTER);
    }

    sc->drawing_area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(sc->window), sc->drawing_area);
    g_signal_connect(sc->window, "realize", G_CALLBACK(on_window_realize), NULL);
    g_signal_connect(sc->drawing_area, "draw", G_CALLBACK(on_draw_event), sc);
    gtk_widget_show_all(sc->window);
    play_startup_audio(base_path);

    FrameLoaderData *loader = g_new0(FrameLoaderData, 1);
    loader->context = sc;
    loader->base_path = g_strdup(base_path);
    loader->width = target_width;
    loader->height = target_height;
    GTask *task = g_task_new(NULL, NULL, on_startup_frames_loaded, sc);
    g_task_set_task_data(task, loader, free_frame_loader_data);
    g_task_run_in_thread(task, load_startup_frames);
    g_object_unref(task);

    g_timeout_add(STARTUP_FRAME_INTERVAL_MS, run_startup_frame, NULL);
}
