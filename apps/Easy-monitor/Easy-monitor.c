#define _DEFAULT_SOURCE
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#define HISTORY_LENGTH 60
#define REFRESH_INTERVAL_MS 1000

typedef struct {
    GtkWidget *cpu_value;
    GtkWidget *cpu_detail;
    GtkWidget *cpu_error;
    GtkWidget *cpu_graph;
    GtkWidget *cpu_page_value;
    GtkWidget *cpu_page_graph;
    GtkWidget *cpu_page_error;
    GtkWidget *memory_value;
    GtkWidget *memory_detail;
    GtkWidget *memory_error;
    GtkWidget *memory_bar;
    GtkWidget *memory_free;
    GtkWidget *memory_page_value;
    GtkWidget *memory_page_detail;
    GtkWidget *memory_page_error;
    GtkWidget *memory_page_bar;
    GtkWidget *storage_value;
    GtkWidget *storage_detail;
    GtkWidget *storage_error;
    GtkWidget *storage_bar;
    GtkWidget *storage_free;
    GtkWidget *storage_page_value;
    GtkWidget *storage_page_detail;
    GtkWidget *storage_page_error;
    GtkWidget *storage_page_bar;
    GtkWidget *updated;
    GtkWidget *stack;
    GtkWidget *nav_buttons[4];
    GtkWidget *cpu_model;
    GtkWidget *cpu_cores;
    GtkWidget *memory_total;
    GtkWidget *memory_available;
    GtkWidget *swap_detail;
    GtkWidget *storage_mount;
    GtkWidget *storage_filesystem;
    gdouble history[HISTORY_LENGTH];
    guint history_count;
    gboolean have_cpu_sample;
    guint64 previous_total;
    guint64 previous_idle;
    gdouble cpu_percent;
    guint64 memory_total_kb;
    guint64 memory_available_kb;
    guint64 swap_total_kb;
    guint64 swap_free_kb;
    guint64 storage_total;
    guint64 storage_used;
    gboolean memory_ok;
    gboolean storage_ok;
} Monitor;

static const gchar *page_names[] = {"overview", "cpu", "memory", "storage"};
static const gchar *page_titles[] = {"Overview", "CPU", "Memory", "Storage"};
static const gchar *nav_icons[] = {
    "view-grid-symbolic", "utilities-system-monitor-symbolic",
    "drive-harddisk-symbolic", "drive-harddisk-symbolic"
};

static gchar *format_bytes(guint64 bytes)
{
    const gchar *units[] = {"B", "KB", "MB", "GB", "TB"};
    gdouble amount = (gdouble)bytes;
    guint unit = 0;
    while (amount >= 1024.0 && unit < G_N_ELEMENTS(units) - 1) {
        amount /= 1024.0;
        unit++;
    }
    return g_strdup_printf(unit == 0 ? "%.0f %s" : "%.1f %s",
                           amount, units[unit]);
}

static gchar *format_kilobytes(guint64 kilobytes)
{
    return format_bytes(kilobytes * 1024);
}

static GtkWidget *new_label(const gchar *text, const gchar *style)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    if (style)
        gtk_style_context_add_class(gtk_widget_get_style_context(label), style);
    return label;
}

static GtkWidget *make_card(const gchar *title, GtkWidget **content_box)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "card");
    gtk_container_set_border_width(GTK_CONTAINER(frame), 17);
    GtkWidget *heading = new_label(title, "card-title");
    gtk_box_pack_start(GTK_BOX(frame), heading, FALSE, FALSE, 0);
    *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_box_pack_start(GTK_BOX(frame), *content_box, TRUE, TRUE, 0);
    return frame;
}

static GtkWidget *make_page_heading(const gchar *title, const gchar *subtitle)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_bottom(box, 18);
    GtkWidget *title_label = new_label(title, "page-title");
    GtkWidget *subtitle_label = new_label(subtitle, "page-subtitle");
    gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), subtitle_label, FALSE, FALSE, 0);
    return box;
}

static gboolean read_cpu(Monitor *monitor, gchar **error_message)
{
    FILE *file = fopen("/proc/stat", "r");
    if (!file) {
        *error_message = g_strdup("Unable to read /proc/stat");
        return FALSE;
    }

    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    int fields = fscanf(file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &user, &nice, &system, &idle, &iowait, &irq,
                        &softirq, &steal);
    fclose(file);
    if (fields < 4) {
        *error_message = g_strdup("CPU statistics are unavailable");
        return FALSE;
    }

    guint64 idle_all = idle + iowait;
    guint64 total = user + nice + system + idle + iowait +
                    irq + softirq + steal;
    if (monitor->have_cpu_sample && total > monitor->previous_total) {
        guint64 total_delta = total - monitor->previous_total;
        guint64 idle_delta = idle_all >= monitor->previous_idle
                                 ? idle_all - monitor->previous_idle : 0;
        monitor->cpu_percent = 100.0 * (gdouble)(total_delta - MIN(total_delta, idle_delta)) /
                               (gdouble)total_delta;
        monitor->cpu_percent = CLAMP(monitor->cpu_percent, 0.0, 100.0);
        if (monitor->history_count < HISTORY_LENGTH)
            monitor->history[monitor->history_count++] = monitor->cpu_percent;
        else {
            memmove(monitor->history, monitor->history + 1,
                    sizeof(gdouble) * (HISTORY_LENGTH - 1));
            monitor->history[HISTORY_LENGTH - 1] = monitor->cpu_percent;
        }
    }
    monitor->previous_total = total;
    monitor->previous_idle = idle_all;
    monitor->have_cpu_sample = TRUE;
    return TRUE;
}

static gboolean read_memory(Monitor *monitor, gchar **error_message)
{
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) {
        *error_message = g_strdup("Unable to read /proc/meminfo");
        return FALSE;
    }
    guint64 total = 0, available = 0, free_mem = 0, buffers = 0;
    guint64 cached = 0, swap_total = 0, swap_free = 0;
    gchar line[256];
    while (fgets(line, sizeof(line), file)) {
        unsigned long long parsed = 0;
        if (sscanf(line, "MemTotal: %llu", &parsed) == 1)
            total = parsed;
        else if (sscanf(line, "MemAvailable: %llu", &parsed) == 1)
            available = parsed;
        else if (sscanf(line, "MemFree: %llu", &parsed) == 1)
            free_mem = parsed;
        else if (sscanf(line, "Buffers: %llu", &parsed) == 1)
            buffers = parsed;
        else if (sscanf(line, "Cached: %llu", &parsed) == 1)
            cached = parsed;
        else if (sscanf(line, "SwapTotal: %llu", &parsed) == 1)
            swap_total = parsed;
        else if (sscanf(line, "SwapFree: %llu", &parsed) == 1)
            swap_free = parsed;
    }
    fclose(file);
    if (total == 0) {
        *error_message = g_strdup("Memory statistics are incomplete");
        return FALSE;
    }
    if (available == 0)
        available = MIN(total, free_mem + buffers + cached);
    monitor->memory_total_kb = total;
    monitor->memory_available_kb = MIN(available, total);
    monitor->swap_total_kb = swap_total;
    monitor->swap_free_kb = MIN(swap_free, swap_total);
    return TRUE;
}

static gboolean read_storage(Monitor *monitor, gchar **error_message)
{
    struct statvfs info;
    if (statvfs("/", &info) != 0) {
        *error_message = g_strdup("Unable to read filesystem usage for /");
        return FALSE;
    }
    if (info.f_blocks == 0 || info.f_frsize == 0) {
        *error_message = g_strdup("Filesystem capacity is unavailable");
        return FALSE;
    }
    monitor->storage_total = (guint64)info.f_blocks * info.f_frsize;
    monitor->storage_used = (guint64)(info.f_blocks - info.f_bfree) * info.f_frsize;
    monitor->storage_used = MIN(monitor->storage_used, monitor->storage_total);
    return TRUE;
}

static gboolean draw_cpu_graph(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Monitor *monitor = data;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    gdouble width = allocation.width;
    gdouble height = allocation.height;
    const gdouble left = 38.0, right = 12.0, top = 12.0, bottom = 24.0;
    gdouble chart_width = MAX(1.0, width - left - right);
    gdouble chart_height = MAX(1.0, height - top - bottom);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_select_font_face(cr, "Cantarell", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    const gdouble levels[] = {0.0, 0.5, 1.0};
    for (guint i = 0; i < G_N_ELEMENTS(levels); i++) {
        gdouble y = top + chart_height * levels[i];
        cairo_set_source_rgba(cr, 0.18, 0.20, 0.24, 0.10);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, left, y);
        cairo_line_to(cr, width - right, y);
        cairo_stroke(cr);
        gchar *text = g_strdup_printf("%d%%", 100 - (gint)(levels[i] * 100));
        cairo_set_source_rgb(cr, 0.52, 0.52, 0.56);
        cairo_move_to(cr, 2.0, y + 3.5);
        cairo_show_text(cr, text);
        g_free(text);
    }
    if (monitor->history_count == 0)
        return FALSE;

    gdouble step = chart_width / MAX(1, HISTORY_LENGTH - 1);
    guint padding = HISTORY_LENGTH - monitor->history_count;
    cairo_new_path(cr);
    for (guint i = 0; i < monitor->history_count; i++) {
        gdouble x = left + (padding + i) * step;
        gdouble y = top + chart_height * (1.0 - monitor->history[i] / 100.0);
        if (i == 0)
            cairo_move_to(cr, x, y);
        else
            cairo_line_to(cr, x, y);
    }
    cairo_line_to(cr, left + (padding + monitor->history_count - 1) * step,
                  top + chart_height);
    cairo_line_to(cr, left + padding * step, top + chart_height);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.08, 0.48, 0.96, 0.12);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.08, 0.48, 0.96);
    cairo_set_line_width(cr, 2.3);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    return FALSE;
}

static void append_error(GtkWidget *label, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(label), message ? message : "");
    gtk_widget_set_visible(label, message != NULL);
}

static void update_monitor(gpointer data)
{
    Monitor *monitor = data;
    gchar *error = NULL;
    if (read_cpu(monitor, &error)) {
        gchar *value = monitor->history_count
                           ? g_strdup_printf("%.0f%%", monitor->cpu_percent)
                           : g_strdup("Sampling…");
        gtk_label_set_text(GTK_LABEL(monitor->cpu_value), value);
        gtk_label_set_text(GTK_LABEL(monitor->cpu_page_value), value);
        g_free(value);
        const gchar *cpu_description = monitor->history_count
                                           ? "Total processor utilization"
                                           : "Waiting for the first sample";
        gtk_label_set_text(GTK_LABEL(monitor->cpu_detail), cpu_description);
        append_error(monitor->cpu_error, NULL);
        append_error(monitor->cpu_page_error, NULL);
        gtk_widget_queue_draw(monitor->cpu_graph);
        gtk_widget_queue_draw(monitor->cpu_page_graph);
    } else {
        append_error(monitor->cpu_error, error);
        append_error(monitor->cpu_page_error, error);
    }
    g_free(error);

    error = NULL;
    monitor->memory_ok = read_memory(monitor, &error);
    if (monitor->memory_ok) {
        guint64 used_kb = monitor->memory_total_kb - monitor->memory_available_kb;
        gdouble percent = 100.0 * (gdouble)used_kb / monitor->memory_total_kb;
        gchar *used = format_kilobytes(used_kb);
        gchar *total = format_kilobytes(monitor->memory_total_kb);
        gchar *available = format_kilobytes(monitor->memory_available_kb);
        gchar *swap_used = format_kilobytes(monitor->swap_total_kb -
                                            monitor->swap_free_kb);
        gchar *swap_total = format_kilobytes(monitor->swap_total_kb);
        gchar *value = g_strdup_printf("%.0f%%", percent);
        gchar *detail = g_strdup_printf("%s of %s used", used, total);
        gchar *swap = monitor->swap_total_kb
                          ? g_strdup_printf("%s of %s swap used", swap_used, swap_total)
                          : g_strdup("No swap configured");
        gtk_label_set_text(GTK_LABEL(monitor->memory_value), value);
        gtk_label_set_text(GTK_LABEL(monitor->memory_detail), detail);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(monitor->memory_bar),
                                      percent / 100.0);
        gtk_label_set_text(GTK_LABEL(monitor->memory_page_value), value);
        gtk_label_set_text(GTK_LABEL(monitor->memory_page_detail), detail);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(monitor->memory_page_bar),
                                      percent / 100.0);
        gtk_label_set_text(GTK_LABEL(monitor->memory_free), available);
        gtk_label_set_text(GTK_LABEL(monitor->memory_total), total);
        gtk_label_set_text(GTK_LABEL(monitor->memory_available), available);
        gtk_label_set_text(GTK_LABEL(monitor->swap_detail), swap);
        append_error(monitor->memory_error, NULL);
        append_error(monitor->memory_page_error, NULL);
        g_free(used);
        g_free(total);
        g_free(available);
        g_free(swap_used);
        g_free(swap_total);
        g_free(value);
        g_free(detail);
        g_free(swap);
    } else {
        append_error(monitor->memory_error, error);
        append_error(monitor->memory_page_error, error);
    }
    g_free(error);

    error = NULL;
    monitor->storage_ok = read_storage(monitor, &error);
    if (monitor->storage_ok) {
        guint64 free_bytes = monitor->storage_total - monitor->storage_used;
        gdouble percent = 100.0 * (gdouble)monitor->storage_used /
                          monitor->storage_total;
        gchar *used = format_bytes(monitor->storage_used);
        gchar *total = format_bytes(monitor->storage_total);
        gchar *free_space = format_bytes(free_bytes);
        gchar *value = g_strdup_printf("%.0f%%", percent);
        gchar *detail = g_strdup_printf("%s of %s used on /", used, total);
        gtk_label_set_text(GTK_LABEL(monitor->storage_value), value);
        gtk_label_set_text(GTK_LABEL(monitor->storage_detail), detail);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(monitor->storage_bar),
                                      percent / 100.0);
        gtk_label_set_text(GTK_LABEL(monitor->storage_page_value), value);
        gtk_label_set_text(GTK_LABEL(monitor->storage_page_detail), detail);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(monitor->storage_page_bar),
                                      percent / 100.0);
        gtk_label_set_text(GTK_LABEL(monitor->storage_free), free_space);
        gtk_label_set_text(GTK_LABEL(monitor->storage_filesystem), total);
        gtk_label_set_text(GTK_LABEL(monitor->storage_mount), "/");
        append_error(monitor->storage_error, NULL);
        append_error(monitor->storage_page_error, NULL);
        g_free(used);
        g_free(total);
        g_free(free_space);
        g_free(value);
        g_free(detail);
    } else {
        append_error(monitor->storage_error, error);
        append_error(monitor->storage_page_error, error);
    }
    g_free(error);

    GDateTime *now = g_date_time_new_now_local();
    gchar *clock = g_date_time_format(now, "%H:%M:%S");
    gchar *time = g_strdup_printf("Updated at %s", clock ? clock : "—");
    gtk_label_set_text(GTK_LABEL(monitor->updated), time);
    g_date_time_unref(now);
    g_free(clock);
    g_free(time);
}

static gboolean refresh_timeout(gpointer data)
{
    update_monitor(data);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *make_cpu_chart(Monitor *monitor, gint height)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, height);
    gtk_widget_set_hexpand(area, TRUE);
    g_signal_connect(area, "draw", G_CALLBACK(draw_cpu_graph), monitor);
    return area;
}

static GtkWidget *make_overview(Monitor *monitor)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(page), "page");
    gtk_container_set_border_width(GTK_CONTAINER(page), 26);
    gtk_box_pack_start(GTK_BOX(page),
                       make_page_heading("Activity", "A live view of your system"),
                       FALSE, FALSE, 0);

    GtkWidget *cpu_card_content;
    GtkWidget *cpu_card = make_card("CPU", &cpu_card_content);
    GtkWidget *cpu_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    monitor->cpu_value = new_label("Sampling…", "metric-value");
    gtk_widget_set_size_request(monitor->cpu_value, 98, -1);
    GtkWidget *cpu_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    monitor->cpu_detail = new_label("Waiting for the first sample", "metric-secondary");
    monitor->cpu_error = new_label("", "error-label");
    gtk_box_pack_start(GTK_BOX(cpu_text), monitor->cpu_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_text), monitor->cpu_error, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_row), monitor->cpu_value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_row), cpu_text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_card_content), cpu_row, FALSE, FALSE, 0);
    monitor->cpu_graph = make_cpu_chart(monitor, 142);
    gtk_box_pack_start(GTK_BOX(cpu_card_content), monitor->cpu_graph, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), cpu_card, FALSE, FALSE, 0);

    GtkWidget *summary_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_top(summary_row, 14);
    GtkWidget *memory_content;
    GtkWidget *memory_card = make_card("MEMORY", &memory_content);
    monitor->memory_value = new_label("--", "metric-value");
    monitor->memory_detail = new_label("Reading memory information…", "metric-secondary");
    monitor->memory_error = new_label("", "error-label");
    monitor->memory_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(monitor->memory_bar), "memory");
    monitor->memory_free = new_label("-- available", "muted-label");
    gtk_box_pack_start(GTK_BOX(memory_content), monitor->memory_value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(memory_content), monitor->memory_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(memory_content), monitor->memory_bar, FALSE, FALSE, 3);
    gtk_box_pack_start(GTK_BOX(memory_content), monitor->memory_free, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(memory_content), monitor->memory_error, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(summary_row), memory_card, TRUE, TRUE, 0);

    GtkWidget *storage_content;
    GtkWidget *storage_card = make_card("STORAGE", &storage_content);
    monitor->storage_value = new_label("--", "metric-value");
    monitor->storage_detail = new_label("Checking system volume…", "metric-secondary");
    monitor->storage_error = new_label("", "error-label");
    monitor->storage_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(monitor->storage_bar), "storage");
    monitor->storage_free = new_label("-- available", "muted-label");
    gtk_box_pack_start(GTK_BOX(storage_content), monitor->storage_value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(storage_content), monitor->storage_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(storage_content), monitor->storage_bar, FALSE, FALSE, 3);
    gtk_box_pack_start(GTK_BOX(storage_content), monitor->storage_free, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(storage_content), monitor->storage_error, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(summary_row), storage_card, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), summary_row, FALSE, FALSE, 0);
    return page;
}

static GtkWidget *make_cpu_page(Monitor *monitor)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(page), "page");
    gtk_container_set_border_width(GTK_CONTAINER(page), 26);
    gtk_box_pack_start(GTK_BOX(page),
                       make_page_heading("CPU", "Processor activity over time"),
                       FALSE, FALSE, 0);
    GtkWidget *content;
    GtkWidget *card = make_card("TOTAL UTILIZATION", &content);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    monitor->cpu_page_value = new_label("Sampling…", "metric-value");
    gtk_widget_set_size_request(monitor->cpu_page_value, 100, -1);
    monitor->cpu_page_error = new_label("", "error-label");
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    monitor->cpu_model = new_label("System processor", "detail-value");
    monitor->cpu_cores = new_label("Logical processors", "muted-label");
    gtk_box_pack_start(GTK_BOX(info), monitor->cpu_model, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info), monitor->cpu_cores, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info), monitor->cpu_page_error, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), monitor->cpu_page_value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), info, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, FALSE, 0);
    monitor->cpu_page_graph = make_cpu_chart(monitor, 330);
    gtk_box_pack_start(GTK_BOX(content), monitor->cpu_page_graph, TRUE, TRUE, 8);
    GtkWidget *note = new_label("Samples the Linux kernel CPU counters once per second.", "footer-note");
    gtk_box_pack_start(GTK_BOX(content), note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, TRUE, TRUE, 0);
    return page;
}

static GtkWidget *make_memory_page(Monitor *monitor)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(page), "page");
    gtk_container_set_border_width(GTK_CONTAINER(page), 26);
    gtk_box_pack_start(GTK_BOX(page),
                       make_page_heading("Memory", "Physical memory and swap"),
                       FALSE, FALSE, 0);
    GtkWidget *content;
    GtkWidget *card = make_card("MEMORY PRESSURE", &content);
    monitor->memory_page_value = new_label("--", "metric-value");
    monitor->memory_page_detail = new_label("Reading memory information…", "metric-secondary");
    monitor->memory_page_error = new_label("", "error-label");
    monitor->memory_page_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(monitor->memory_page_bar), "memory");
    gtk_widget_set_margin_top(monitor->memory_page_bar, 8);
    gtk_widget_set_margin_bottom(monitor->memory_page_bar, 14);
    gtk_box_pack_start(GTK_BOX(content), monitor->memory_page_value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), monitor->memory_page_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), monitor->memory_page_bar, FALSE, FALSE, 0);
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
    GtkWidget *total_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(total_box), new_label("TOTAL MEMORY", "card-title"), FALSE, FALSE, 0);
    monitor->memory_total = new_label("--", "detail-value");
    gtk_box_pack_start(GTK_BOX(total_box), monitor->memory_total, FALSE, FALSE, 0);
    GtkWidget *available_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(available_box), new_label("AVAILABLE", "card-title"), FALSE, FALSE, 0);
    monitor->memory_available = new_label("--", "detail-value");
    gtk_box_pack_start(GTK_BOX(available_box), monitor->memory_available, FALSE, FALSE, 0);
    GtkWidget *swap_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(swap_box), new_label("SWAP", "card-title"), FALSE, FALSE, 0);
    monitor->swap_detail = new_label("--", "detail-value");
    gtk_box_pack_start(GTK_BOX(swap_box), monitor->swap_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), total_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(details), available_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(details), swap_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), monitor->memory_page_error, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, TRUE, TRUE, 0);
    return page;
}

static GtkWidget *make_storage_page(Monitor *monitor)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(page), "page");
    gtk_container_set_border_width(GTK_CONTAINER(page), 26);
    gtk_box_pack_start(GTK_BOX(page),
                       make_page_heading("Storage", "Capacity of your system volume"),
                       FALSE, FALSE, 0);
    GtkWidget *content;
    GtkWidget *card = make_card("SYSTEM VOLUME", &content);
    monitor->storage_page_value = new_label("--", "metric-value");
    monitor->storage_page_detail = new_label("Checking system volume…", "metric-secondary");
    monitor->storage_page_error = new_label("", "error-label");
    monitor->storage_page_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(monitor->storage_page_bar), "storage");
    gtk_widget_set_margin_top(monitor->storage_page_bar, 8);
    gtk_widget_set_margin_bottom(monitor->storage_page_bar, 14);
    gtk_box_pack_start(GTK_BOX(content), monitor->storage_page_value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), monitor->storage_page_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), monitor->storage_page_bar, FALSE, FALSE, 0);
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
    GtkWidget *mount_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(mount_box), new_label("MOUNT POINT", "card-title"), FALSE, FALSE, 0);
    monitor->storage_mount = new_label("/", "detail-value");
    gtk_box_pack_start(GTK_BOX(mount_box), monitor->storage_mount, FALSE, FALSE, 0);
    GtkWidget *capacity_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(capacity_box), new_label("CAPACITY", "card-title"), FALSE, FALSE, 0);
    monitor->storage_filesystem = new_label("--", "detail-value");
    gtk_box_pack_start(GTK_BOX(capacity_box), monitor->storage_filesystem, FALSE, FALSE, 0);
    GtkWidget *free_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(free_box), new_label("FREE SPACE", "card-title"), FALSE, FALSE, 0);
    monitor->storage_free = new_label("--", "detail-value");
    gtk_box_pack_start(GTK_BOX(free_box), monitor->storage_free, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), mount_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(details), capacity_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(details), free_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), monitor->storage_page_error, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, TRUE, TRUE, 0);
    return page;
}

static void nav_clicked(GtkButton *button, gpointer user_data)
{
    Monitor *monitor = user_data;
    gint index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "page-index"));
    gtk_stack_set_visible_child_name(GTK_STACK(monitor->stack), page_names[index]);
    for (guint i = 0; i < G_N_ELEMENTS(monitor->nav_buttons); i++) {
        GtkStyleContext *context = gtk_widget_get_style_context(monitor->nav_buttons[i]);
        if ((gint)i == index)
            gtk_style_context_add_class(context, "active");
        else
            gtk_style_context_remove_class(context, "active");
    }
    GtkWidget *title = g_object_get_data(G_OBJECT(monitor->stack), "window-title");
    gtk_label_set_text(GTK_LABEL(title), page_titles[index]);
}

static gchar *read_cpu_model(void)
{
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (!file)
        return g_strdup("System processor");
    gchar line[512];
    gchar *model = NULL;
    while (fgets(line, sizeof(line), file)) {
        if (g_str_has_prefix(line, "model name") ||
            g_str_has_prefix(line, "Hardware")) {
            gchar *colon = strchr(line, ':');
            if (colon) {
                gchar *value = g_strstrip(colon + 1);
                model = g_strdup(value);
                break;
            }
        }
    }
    fclose(file);
    return model ? model : g_strdup("System processor");
}

static void load_css(void)
{
    gchar *executable = g_file_read_link("/proc/self/exe", NULL);
    gchar *directory = executable ? g_path_get_dirname(executable) : g_get_current_dir();
    gchar *path = g_build_filename(directory, "Easy-monitor.css", NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        if (!gtk_css_provider_load_from_path(provider, path, &error)) {
            g_warning("Could not load %s: %s", path, error->message);
            g_clear_error(&error);
        }
    } else {
        g_warning("Stylesheet not found: %s", path);
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_free(path);
    g_free(directory);
    g_free(executable);
}

static void build_ui(Monitor *monitor)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Easy Monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 700);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "easy-monitor");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), layout);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar), "sidebar");
    gtk_container_set_border_width(GTK_CONTAINER(sidebar), 15);
    gtk_widget_set_size_request(sidebar, 190, -1);
    gtk_box_pack_start(GTK_BOX(layout), sidebar, FALSE, FALSE, 0);

    GtkWidget *brand = new_label("◉  Easy Monitor", "sidebar-brand");
    gtk_box_pack_start(GTK_BOX(sidebar), brand, FALSE, FALSE, 0);
    GtkWidget *section = new_label("MONITOR", "sidebar-section");
    gtk_box_pack_start(GTK_BOX(sidebar), section, FALSE, FALSE, 0);
    const gchar *nav_names[] = {"Overview", "CPU", "Memory", "Storage"};
    for (guint i = 0; i < G_N_ELEMENTS(nav_names); i++) {
        GtkWidget *button = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "nav-button");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *icon = gtk_image_new_from_icon_name(nav_icons[i], GTK_ICON_SIZE_MENU);
        GtkWidget *label = gtk_label_new(nav_names[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(button), row);
        g_object_set_data(G_OBJECT(button), "page-index", GINT_TO_POINTER(i));
        g_signal_connect(button, "clicked", G_CALLBACK(nav_clicked), monitor);
        monitor->nav_buttons[i] = button;
        gtk_box_pack_start(GTK_BOX(sidebar), button, FALSE, FALSE, 0);
    }
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), spacer, TRUE, TRUE, 0);
    GtkWidget *footer = new_label("Live system statistics", "footer-note");
    gtk_box_pack_start(GTK_BOX(sidebar), footer, FALSE, FALSE, 0);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(layout), body, TRUE, TRUE, 0);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "toolbar");
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 15);
    GtkWidget *title = new_label("Overview", "window-title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    monitor->updated = new_label("Starting monitor…", "updated-label");
    gtk_widget_set_halign(monitor->updated, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(toolbar), monitor->updated, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), toolbar, FALSE, FALSE, 0);

    monitor->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(monitor->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(monitor->stack), 140);
    g_object_set_data(G_OBJECT(monitor->stack), "window-title", title);
    gtk_box_pack_start(GTK_BOX(body), monitor->stack, TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(monitor->stack), make_overview(monitor), "overview");
    gtk_stack_add_named(GTK_STACK(monitor->stack), make_cpu_page(monitor), "cpu");
    gtk_stack_add_named(GTK_STACK(monitor->stack), make_memory_page(monitor), "memory");
    gtk_stack_add_named(GTK_STACK(monitor->stack), make_storage_page(monitor), "storage");
    gtk_style_context_add_class(gtk_widget_get_style_context(monitor->nav_buttons[0]), "active");
    gtk_widget_show_all(window);
    gtk_widget_hide(monitor->cpu_error);
    gtk_widget_hide(monitor->cpu_page_error);
    gtk_widget_hide(monitor->memory_error);
    gtk_widget_hide(monitor->memory_page_error);
    gtk_widget_hide(monitor->storage_error);
    gtk_widget_hide(monitor->storage_page_error);
    gtk_widget_show(window);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    load_css();
    Monitor monitor = {0};
    gchar *model = read_cpu_model();
    build_ui(&monitor);
    gtk_label_set_text(GTK_LABEL(monitor.cpu_model), model);
    g_free(model);
    gchar *cores = g_strdup_printf("%u logical processors", g_get_num_processors());
    gtk_label_set_text(GTK_LABEL(monitor.cpu_cores), cores);
    g_free(cores);
    update_monitor(&monitor);
    g_timeout_add(REFRESH_INTERVAL_MS, refresh_timeout, &monitor);
    gtk_main();
    return 0;
}
