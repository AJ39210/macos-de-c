#define _GNU_SOURCE
#include "stats.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *cpu;
    GtkWidget *memory;
    GtkWidget *gpu;
    guint64 previous_total;
    guint64 previous_idle;
    guint update_source;
} StatsWidget;

static gboolean
read_cpu_counters(guint64 *total, guint64 *idle)
{
    FILE *file = fopen("/proc/stat", "r");
    if (file == NULL)
        return FALSE;

    guint64 user, nice, system, idle_time, io_wait, irq, soft_irq, steal;
    int fields = fscanf(file, "cpu %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                        " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                        " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                        " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
                        &user, &nice, &system, &idle_time, &io_wait, &irq,
                        &soft_irq, &steal);
    fclose(file);
    if (fields < 4)
        return FALSE;
    *idle = idle_time + io_wait;
    *total = user + nice + system + idle_time + io_wait + irq + soft_irq + steal;
    return TRUE;
}

static gchar *
read_memory_usage(void)
{
    gchar *contents = NULL;
    if (!g_file_get_contents("/proc/meminfo", &contents, NULL, NULL))
        return g_strdup("Memory: unavailable");

    guint64 total_kb = 0;
    guint64 available_kb = 0;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (sscanf(lines[i], "MemTotal: %" G_GUINT64_FORMAT " kB",
                   &total_kb) == 1)
            continue;
        if (sscanf(lines[i], "MemAvailable: %" G_GUINT64_FORMAT " kB",
                   &available_kb) == 1)
            break;
    }
    g_strfreev(lines);
    g_free(contents);
    if (total_kb == 0 || available_kb > total_kb)
        return g_strdup("Memory: unavailable");

    guint64 used_kb = total_kb - available_kb;
    return g_strdup_printf("RAM  %.1f / %.1f GB  (%.0f%%)",
                           (gdouble)used_kb / 1048576.0,
                           (gdouble)total_kb / 1048576.0,
                           100.0 * (gdouble)used_kb / (gdouble)total_kb);
}

static gchar *
read_gpu_usage(void)
{
    GError *error = NULL;
    GDir *directory = g_dir_open("/sys/class/drm", 0, &error);
    if (directory == NULL) {
        g_clear_error(&error);
        return g_strdup("GPU  not detected");
    }

    gchar *usage = NULL;
    const gchar *entry;
    while ((entry = g_dir_read_name(directory)) != NULL) {
        if (!g_str_has_prefix(entry, "card") ||
            !g_ascii_isdigit(entry[4]))
            continue;
        const gchar *numeric_end = entry + 4;
        while (*numeric_end && g_ascii_isdigit(*numeric_end))
            numeric_end++;
        if (*numeric_end != '\0')
            continue;
        gchar *path = g_build_filename("/sys/class/drm", entry, "device",
                                       "gpu_busy_percent", NULL);
        gchar *contents = NULL;
        if (g_file_get_contents(path, &contents, NULL, NULL)) {
            gchar *end = NULL;
            errno = 0;
            long value = strtol(contents, &end, 10);
            if (errno == 0 && end != contents && value >= 0 && value <= 100)
                usage = g_strdup_printf("GPU  %ld%%", value);
            g_free(contents);
        }
        g_free(path);
        if (usage != NULL)
            break;
    }
    g_dir_close(directory);
    return usage ? usage : g_strdup("GPU  usage unavailable");
}

static gboolean
update_stats(gpointer user_data)
{
    StatsWidget *stats = user_data;
    guint64 total, idle;
    if (read_cpu_counters(&total, &idle)) {
        if (stats->previous_total != 0 && total >= stats->previous_total &&
            idle >= stats->previous_idle) {
            guint64 total_delta = total - stats->previous_total;
            guint64 idle_delta = idle - stats->previous_idle;
            gdouble percent = total_delta == 0 ? 0.0 :
                100.0 * (gdouble)(total_delta - MIN(total_delta, idle_delta)) /
                (gdouble)total_delta;
            gchar *text = g_strdup_printf("CPU  %.0f%%", percent);
            gtk_label_set_text(GTK_LABEL(stats->cpu), text);
            g_free(text);
        }
        stats->previous_total = total;
        stats->previous_idle = idle;
    } else {
        gtk_label_set_text(GTK_LABEL(stats->cpu), "CPU  unavailable");
    }

    gchar *memory = read_memory_usage();
    gchar *gpu = read_gpu_usage();
    gtk_label_set_text(GTK_LABEL(stats->memory), memory);
    gtk_label_set_text(GTK_LABEL(stats->gpu), gpu);
    g_free(memory);
    g_free(gpu);
    return G_SOURCE_CONTINUE;
}

static void
free_stats(GtkWidget *widget, gpointer user_data)
{
    StatsWidget *stats = user_data;
    (void)widget;
    if (stats->update_source != 0)
        g_source_remove(stats->update_source);
    g_free(stats);
}

GtkWidget *
macos_de_stats_widget_create(void)
{
    StatsWidget *stats = g_new0(StatsWidget, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *title = gtk_label_new("System Stats");
    stats->cpu = gtk_label_new("CPU  measuring…");
    stats->memory = gtk_label_new("RAM  measuring…");
    stats->gpu = gtk_label_new("GPU  checking…");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(box),
                                "system-stats-widget");
    gtk_style_context_add_class(gtk_widget_get_style_context(title),
                                "system-stats-title");
    GtkWidget *labels[] = { stats->cpu, stats->memory, stats->gpu };
    for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
        gtk_widget_set_halign(labels[i], GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(labels[i]),
                                    "system-stats-label");
        gtk_box_pack_start(GTK_BOX(box), labels[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(box), title, 0);
    g_signal_connect(box, "destroy", G_CALLBACK(free_stats), stats);
    stats->update_source = g_timeout_add_seconds(1, update_stats, stats);

    const gchar *css_candidates[] = {
        "/usr/share/macos-de/widgets/stats/style.css",
        "build/widgets/stats/style.css",
        "widgets/stats/style.css",
        NULL
    };
    GtkCssProvider *provider = gtk_css_provider_new();
    gboolean loaded = FALSE;
    for (guint i = 0; css_candidates[i] != NULL && !loaded; i++) {
        if (g_file_test(css_candidates[i], G_FILE_TEST_IS_REGULAR)) {
            GError *error = NULL;
            loaded = gtk_css_provider_load_from_path(
                provider, css_candidates[i], &error);
            if (!loaded) {
                g_warning("Could not load stats widget styling from %s: %s",
                          css_candidates[i], error->message);
                g_clear_error(&error);
            }
        }
    }
    if (loaded)
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    return box;
}
