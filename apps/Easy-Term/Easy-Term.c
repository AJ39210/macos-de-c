#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>

typedef struct _VteTerminal VteTerminal;
typedef enum {
    VTE_CURSOR_BLINK_SYSTEM = 0,
    VTE_CURSOR_BLINK_ON = 1,
    VTE_CURSOR_BLINK_OFF = 2
} VteCursorBlinkMode;

typedef GtkWidget *(*VteTerminalNewFunc)(void);
typedef gboolean (*VteTerminalSpawnSyncFunc)(
    VteTerminal *, int, const char *, char **, char **, GSpawnFlags,
    GSpawnChildSetupFunc, gpointer, GPid *, GCancellable *, GError **);
typedef void (*VteTerminalSetFontFunc)(VteTerminal *, const PangoFontDescription *);
typedef void (*VteTerminalSetColorsFunc)(VteTerminal *, const GdkRGBA *,
                                          const GdkRGBA *, const GdkRGBA *, gsize);
typedef void (*VteTerminalSetBlinkFunc)(VteTerminal *, VteCursorBlinkMode);
typedef void (*VteTerminalSetScrollbackFunc)(VteTerminal *, glong);
typedef void (*VteTerminalClipboardFunc)(VteTerminal *);
typedef void (*VteTerminalSelectAllFunc)(VteTerminal *);
typedef void (*VteTerminalUnselectAllFunc)(VteTerminal *);

typedef struct {
    void *handle;
    VteTerminalNewFunc terminal_new;
    VteTerminalSpawnSyncFunc spawn_sync;
    VteTerminalSetFontFunc set_font;
    VteTerminalSetColorsFunc set_colors;
    VteTerminalSetBlinkFunc set_blink;
    VteTerminalSetScrollbackFunc set_scrollback;
    VteTerminalClipboardFunc copy;
    VteTerminalClipboardFunc paste;
    VteTerminalSelectAllFunc select_all;
    VteTerminalUnselectAllFunc unselect_all;
} VteApi;

typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *settings_panel;
    GtkWidget *status;
    GtkWidget *reset_button;
    GtkWidget *font_button;
    GtkWidget *font_size;
    GtkWidget *foreground;
    GtkWidget *background;
    GtkWidget *blink;
    GtkWidget *shell;
    GtkWidget *scrollback;
    GtkWidget *opacity;
    GtkWidget *theme;
    GKeyFile *config;
    gchar *config_path;
    VteApi vte;
    gboolean loading;
} EasyTerm;

typedef struct {
    EasyTerm *app;
    GtkWidget *terminal;
} TerminalTab;

static EasyTerm app;

static void restore_defaults(GtkWidget *widget, gpointer user_data);

static gboolean
load_vte(VteApi *api, GError **error)
{
    const gchar *libraries[] = { "libvte-2.91.so.0", "libvte-2.91.so", NULL };
    guint i;

    for (i = 0; libraries[i] != NULL && api->handle == NULL; i++)
        api->handle = dlopen(libraries[i], RTLD_NOW | RTLD_LOCAL);
    if (api->handle == NULL) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "VTE 2.91 runtime library was not found. Install the "
                    "system VTE terminal runtime (for example, libvte-2.91-0).");
        return FALSE;
    }

#define LOAD_VTE(field, symbol) do { \
    *(void **) (&api->field) = dlsym(api->handle, symbol); \
    if (api->field == NULL) { \
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, \
                    "The installed VTE library is missing required API %s.", symbol); \
        dlclose(api->handle); \
        api->handle = NULL; \
        return FALSE; \
    } \
} while (0)
    LOAD_VTE(terminal_new, "vte_terminal_new");
    LOAD_VTE(spawn_sync, "vte_terminal_spawn_sync");
    LOAD_VTE(set_font, "vte_terminal_set_font");
    LOAD_VTE(set_colors, "vte_terminal_set_colors");
    LOAD_VTE(set_blink, "vte_terminal_set_cursor_blink_mode");
    LOAD_VTE(set_scrollback, "vte_terminal_set_scrollback_lines");
    LOAD_VTE(copy, "vte_terminal_copy_clipboard");
    LOAD_VTE(paste, "vte_terminal_paste_clipboard");
    LOAD_VTE(select_all, "vte_terminal_select_all");
    LOAD_VTE(unselect_all, "vte_terminal_unselect_all");
#undef LOAD_VTE
    return TRUE;
}

static void
set_status(EasyTerm *state, const gchar *message, gboolean is_error)
{
    gtk_label_set_text(GTK_LABEL(state->status), message);
    gtk_style_context_remove_class(gtk_widget_get_style_context(state->status),
                                   "error");
    if (is_error)
        gtk_style_context_add_class(gtk_widget_get_style_context(state->status),
                                    "error");
}

static void
show_error(EasyTerm *state, const gchar *title, const gchar *message)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(state->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                              message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static gchar *
get_string(EasyTerm *state, const gchar *key, const gchar *fallback)
{
    GError *error = NULL;
    gchar *value = g_key_file_get_string(state->config, "Terminal", key, &error);
    if (error != NULL) {
        g_clear_error(&error);
        return g_strdup(fallback);
    }
    return value;
}

static gint
get_integer(EasyTerm *state, const gchar *key, gint fallback)
{
    GError *error = NULL;
    gint value = g_key_file_get_integer(state->config, "Terminal", key, &error);
    if (error != NULL) {
        g_clear_error(&error);
        return fallback;
    }
    return value;
}

static gboolean
get_boolean(EasyTerm *state, const gchar *key, gboolean fallback)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(state->config, "Terminal", key,
                                             &error);
    if (error != NULL) {
        g_clear_error(&error);
        return fallback;
    }
    return value;
}

static void
save_settings(EasyTerm *state)
{
    gchar *directory = g_path_get_dirname(state->config_path);
    gchar *font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(state->font_button));
    gchar *theme = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(state->theme));
    GdkRGBA color;
    GError *error = NULL;

    if (g_mkdir_with_parents(directory, 0700) != 0) {
        gchar *message = g_strdup_printf("Cannot create settings directory: %s",
                                         g_strerror(errno));
        set_status(state, message, TRUE);
        g_warning("%s", message);
        g_free(message);
        g_free(directory);
        g_free(font);
        g_free(theme);
        return;
    }

    g_key_file_set_string(state->config, "Terminal", "font", font);
    g_key_file_set_integer(state->config, "Terminal", "font-size",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->font_size)));
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(state->foreground), &color);
    gchar *foreground = gdk_rgba_to_string(&color);
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(state->background), &color);
    gchar *background = gdk_rgba_to_string(&color);
    g_key_file_set_string(state->config, "Terminal", "foreground", foreground);
    g_key_file_set_string(state->config, "Terminal", "background", background);
    g_key_file_set_boolean(state->config, "Terminal", "cursor-blink",
        gtk_switch_get_active(GTK_SWITCH(state->blink)));
    g_key_file_set_string(state->config, "Terminal", "shell",
        gtk_entry_get_text(GTK_ENTRY(state->shell)));
    g_key_file_set_integer(state->config, "Terminal", "scrollback",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->scrollback)));
    g_key_file_set_double(state->config, "Terminal", "opacity",
        gtk_range_get_value(GTK_RANGE(state->opacity)));
    g_key_file_set_string(state->config, "Terminal", "theme",
                          theme != NULL ? theme : "Dark");

    gchar *data = g_key_file_to_data(state->config, NULL, &error);
    gboolean saved = data != NULL &&
        g_file_set_contents(state->config_path, data, -1, &error);
    if (saved) {
        set_status(state, "Preferences saved", FALSE);
    } else {
        set_status(state, error != NULL ? error->message : "Could not save preferences",
                   TRUE);
        g_warning("Could not save Easy-Term settings: %s",
                  error != NULL ? error->message : "unknown error");
    }
    g_clear_error(&error);
    g_free(data);
    g_free(directory);
    g_free(font);
    g_free(theme);
    g_free(foreground);
    g_free(background);
}

static TerminalTab *
current_tab(EasyTerm *state)
{
    gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(state->notebook));
    GtkWidget *child = page >= 0
        ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook), page) : NULL;
    return child != NULL ? g_object_get_data(G_OBJECT(child), "easy-term-tab") : NULL;
}

static void
apply_preferences(EasyTerm *state)
{
    gchar *font_name = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(state->font_button));
    gint size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->font_size));
    PangoFontDescription *font = pango_font_description_from_string(font_name);
    pango_font_description_set_size(font, size * PANGO_SCALE);

    GdkRGBA foreground, background;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(state->foreground), &foreground);
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(state->background), &background);
    background.alpha = gtk_range_get_value(GTK_RANGE(state->opacity));
    gboolean blinking = gtk_switch_get_active(GTK_SWITCH(state->blink));
    glong scrollback = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(state->scrollback));

    gint count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(state->notebook));
    for (gint i = 0; i < count; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook), i);
        TerminalTab *tab = g_object_get_data(G_OBJECT(page), "easy-term-tab");
        if (tab == NULL)
            continue;
        state->vte.set_font((VteTerminal *)tab->terminal, font);
        state->vte.set_colors((VteTerminal *)tab->terminal, &foreground, &background,
                              NULL, 0);
        state->vte.set_blink((VteTerminal *)tab->terminal,
            blinking ? VTE_CURSOR_BLINK_ON : VTE_CURSOR_BLINK_OFF);
        state->vte.set_scrollback((VteTerminal *)tab->terminal, scrollback);
    }
    pango_font_description_free(font);
    g_free(font_name);

    gchar *theme = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(state->theme));
    GtkStyleContext *context = gtk_widget_get_style_context(state->window);
    gtk_style_context_remove_class(context, "dark");
    gtk_style_context_remove_class(context, "light");
    gtk_style_context_add_class(context,
        theme != NULL && g_ascii_strcasecmp(theme, "Light") == 0 ? "light" : "dark");
    g_free(theme);
}

static void
on_preference_changed(GtkWidget *widget, gpointer user_data)
{
    (void) widget;
    EasyTerm *state = user_data;
    if (state->loading)
        return;
    apply_preferences(state);
    save_settings(state);
}

static gboolean
on_terminal_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void) widget;
    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    TerminalTab *tab = user_data;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *copy = gtk_menu_item_new_with_label("Copy");
    GtkWidget *paste = gtk_menu_item_new_with_label("Paste");
    GtkWidget *select = gtk_menu_item_new_with_label("Select All");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), paste);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), select);
    g_signal_connect_swapped(copy, "activate", G_CALLBACK(tab->app->vte.copy),
                             tab->terminal);
    g_signal_connect_swapped(paste, "activate", G_CALLBACK(tab->app->vte.paste),
                             tab->terminal);
    g_signal_connect_swapped(select, "activate", G_CALLBACK(tab->app->vte.select_all),
                             tab->terminal);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

static void
free_tab_data(gpointer data)
{
    g_free(data);
}

static void
close_tab(GtkButton *button, gpointer user_data)
{
    (void) button;
    GtkWidget *page = user_data;
    EasyTerm *state = &app;
    gint index = gtk_notebook_page_num(GTK_NOTEBOOK(state->notebook), page);
    if (index >= 0)
        gtk_notebook_remove_page(GTK_NOTEBOOK(state->notebook), index);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(state->notebook)) == 0)
        gtk_widget_destroy(state->window);
}

static GtkWidget *
make_tab_label(GtkWidget *page, const gchar *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(title);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic",
                                                       GTK_ICON_SIZE_MENU);
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "tab-close");
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Close tab");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);
    g_signal_connect(close, "clicked", G_CALLBACK(close_tab), page);
    gtk_widget_show_all(box);
    return box;
}

static void
new_tab(EasyTerm *state)
{
    GtkWidget *terminal = state->vte.terminal_new();
    if (terminal == NULL) {
        show_error(state, "Could not create terminal",
                   "VTE failed to create a terminal widget.");
        return;
    }
    TerminalTab *tab = g_new0(TerminalTab, 1);
    tab->app = state;
    tab->terminal = terminal;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(page), terminal, TRUE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(page), "easy-term-tab", tab, free_tab_data);
    g_signal_connect(terminal, "button-press-event",
                     G_CALLBACK(on_terminal_button_press), tab);

    gchar **argv = NULL;
    gint argc = 0;
    GError *error = NULL;
    const gchar *shell_text = gtk_entry_get_text(GTK_ENTRY(state->shell));
    gchar *shell = g_strdup(shell_text != NULL && *shell_text != '\0'
        ? shell_text : (g_getenv("SHELL") != NULL ? g_getenv("SHELL") : "/bin/sh"));
    if (!g_shell_parse_argv(shell, &argc, &argv, &error) || argc == 0) {
        show_error(state, "Invalid shell command",
            error != NULL ? error->message : "Enter a shell command or executable path.");
        g_clear_error(&error);
        g_free(shell);
        g_strfreev(argv);
        gtk_widget_destroy(page);
        return;
    }
    GCancellable *cancellable = g_cancellable_new();
    gboolean spawned = state->vte.spawn_sync((VteTerminal *)terminal, 0, NULL, argv,
        NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, cancellable, &error);
    g_object_unref(cancellable);
    g_strfreev(argv);
    g_free(shell);
    if (!spawned) {
        gchar *message = g_strdup_printf("Could not launch shell: %s",
            error != NULL ? error->message : "VTE did not start the child process.");
        show_error(state, "Shell launch failed", message);
        g_free(message);
        g_clear_error(&error);
        gtk_widget_destroy(page);
        return;
    }

    gint index = gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), page,
                                           make_tab_label(page, "Terminal"));
    gtk_widget_show_all(page);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(state->notebook), index);
    apply_preferences(state);
    gtk_widget_grab_focus(terminal);
}

static GtkWidget *
add_labeled_control(GtkWidget *box, const gchar *label_text, GtkWidget *control)
{
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 3);
    gtk_box_pack_start(GTK_BOX(box), control, FALSE, FALSE, 2);
    return control;
}

static GtkWidget *
create_settings_panel(EasyTerm *state)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_widget_set_size_request(scroll, 280, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "settings-panel");
    gtk_container_add(GTK_CONTAINER(scroll), box);

    GtkWidget *heading = gtk_label_new("Terminal settings");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),
                                "settings-heading");
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 4);

    state->font_button = gtk_font_button_new();
    gtk_font_button_set_use_size(GTK_FONT_BUTTON(state->font_button), FALSE);
    add_labeled_control(box, "Font family", state->font_button);
    state->font_size = gtk_spin_button_new_with_range(7, 36, 1);
    add_labeled_control(box, "Font size", state->font_size);

    GtkWidget *section = gtk_label_new("Colors & appearance");
    gtk_widget_set_halign(section, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(section),
                                "settings-section");
    gtk_box_pack_start(GTK_BOX(box), section, FALSE, FALSE, 2);
    state->foreground = gtk_color_button_new();
    gtk_color_button_set_title(GTK_COLOR_BUTTON(state->foreground), "Terminal text color");
    add_labeled_control(box, "Text color", state->foreground);
    state->background = gtk_color_button_new();
    gtk_color_button_set_title(GTK_COLOR_BUTTON(state->background), "Terminal background color");
    add_labeled_control(box, "Background color", state->background);
    state->opacity = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.4, 1.0, 0.05);
    gtk_scale_set_draw_value(GTK_SCALE(state->opacity), TRUE);
    gtk_scale_set_digits(GTK_SCALE(state->opacity), 2);
    add_labeled_control(box, "Background opacity", state->opacity);
    state->theme = GTK_WIDGET(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->theme), "Dark");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->theme), "Light");
    add_labeled_control(box, "Window theme", state->theme);

    state->blink = gtk_switch_new();
    gtk_widget_set_halign(state->blink, GTK_ALIGN_START);
    add_labeled_control(box, "Blinking cursor", state->blink);

    section = gtk_label_new("Shell & history");
    gtk_widget_set_halign(section, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(section),
                                "settings-section");
    gtk_box_pack_start(GTK_BOX(box), section, FALSE, FALSE, 2);
    state->shell = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->shell),
                                   "Uses $SHELL, then /bin/sh");
    add_labeled_control(box, "Shell command", state->shell);
    state->scrollback = gtk_spin_button_new_with_range(0, 1000000, 1000);
    add_labeled_control(box, "Scrollback lines", state->scrollback);

    state->reset_button = gtk_button_new_with_label("Restore defaults");
    gtk_box_pack_start(GTK_BOX(box), state->reset_button, FALSE, FALSE, 8);
    gtk_widget_show_all(scroll);
    return scroll;
}

static void
restore_defaults(GtkWidget *widget, gpointer user_data)
{
    (void) widget;
    EasyTerm *state = user_data;
    state->loading = TRUE;
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(state->font_button), "Monospace 12");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->font_size), 12);
    GdkRGBA fg, bg;
    gdk_rgba_parse(&fg, "#e8e8e8");
    gdk_rgba_parse(&bg, "#17191e");
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(state->foreground), &fg);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(state->background), &bg);
    gtk_switch_set_active(GTK_SWITCH(state->blink), FALSE);
    const gchar *shell = g_getenv("SHELL");
    gtk_entry_set_text(GTK_ENTRY(state->shell),
                       shell != NULL && *shell != '\0' ? shell : "/bin/sh");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->scrollback), 10000);
    gtk_range_set_value(GTK_RANGE(state->opacity), 1.0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->theme), 0);
    state->loading = FALSE;
    apply_preferences(state);
    save_settings(state);
}

static gboolean
on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void) widget;
    EasyTerm *state = user_data;
    guint modifiers = event->state & gtk_accelerator_get_default_mod_mask();
    if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
        (event->keyval == GDK_KEY_t || event->keyval == GDK_KEY_T)) {
        new_tab(state);
        return TRUE;
    }
    if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
        (event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W)) {
        TerminalTab *tab = current_tab(state);
        if (tab != NULL)
            close_tab(NULL, gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook),
                gtk_notebook_get_current_page(GTK_NOTEBOOK(state->notebook))));
        return TRUE;
    }
    if (event->keyval == GDK_KEY_F10) {
        gtk_widget_set_visible(state->settings_panel,
                               !gtk_widget_get_visible(state->settings_panel));
        return TRUE;
    }
    return FALSE;
}

static void
toggle_settings(GtkButton *button, gpointer user_data)
{
    (void) button;
    EasyTerm *state = user_data;
    gtk_widget_set_visible(state->settings_panel,
                           !gtk_widget_get_visible(state->settings_panel));
}

static void
load_settings_ui(EasyTerm *state)
{
    gchar *font = get_string(state, "font", "Monospace 12");
    gchar *fg = get_string(state, "foreground", "#e8e8e8");
    gchar *bg = get_string(state, "background", "#17191e");
    const gchar *shell_fallback = g_getenv("SHELL");
    if (shell_fallback == NULL || *shell_fallback == '\0')
        shell_fallback = "/bin/sh";
    gchar *shell = get_string(state, "shell", shell_fallback);
    gchar *theme = get_string(state, "theme", "Dark");
    GdkRGBA color;

    state->loading = TRUE;
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(state->font_button), font);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->font_size),
                               get_integer(state, "font-size", 12));
    if (gdk_rgba_parse(&color, fg))
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(state->foreground), &color);
    if (gdk_rgba_parse(&color, bg))
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(state->background), &color);
    gtk_switch_set_active(GTK_SWITCH(state->blink),
                           get_boolean(state, "cursor-blink", FALSE));
    gtk_entry_set_text(GTK_ENTRY(state->shell), shell);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->scrollback),
                               get_integer(state, "scrollback", 10000));
    GError *error = NULL;
    gdouble opacity = g_key_file_get_double(state->config, "Terminal", "opacity", &error);
    if (error != NULL) {
        g_clear_error(&error);
        opacity = 1.0;
    }
    gtk_range_set_value(GTK_RANGE(state->opacity), opacity);
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->theme),
        g_ascii_strcasecmp(theme, "Light") == 0 ? 1 : 0);
    state->loading = FALSE;

    g_free(font);
    g_free(fg);
    g_free(bg);
    g_free(shell);
    g_free(theme);
}

static void
load_css(const gchar *argv0)
{
    gchar *directory = g_path_get_dirname(argv0);
    gchar *path = g_build_filename(directory, "Easy-Term.css", NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_path(provider, path, &error)) {
        g_warning("Could not load Easy-Term stylesheet %s: %s", path, error->message);
        g_clear_error(&error);
    } else {
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
    g_free(path);
    g_free(directory);
}

static void
on_font_set(GtkFontButton *button, gpointer user_data)
{
    on_preference_changed(GTK_WIDGET(button), user_data);
}

static void
on_switch_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) pspec;
    on_preference_changed(GTK_WIDGET(object), user_data);
}

int
main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GError *error = NULL;
    if (!load_vte(&app.vte, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Easy-Term needs a VTE runtime");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                                  error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
        return 1;
    }

    load_css(argv[0]);
    app.config = g_key_file_new();
    app.config_path = g_build_filename(g_get_user_config_dir(), "macos-de",
                                       "easy-term.ini", NULL);
    if (!g_key_file_load_from_file(app.config, app.config_path, G_KEY_FILE_NONE,
                                   &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_warning("Could not read Easy-Term preferences %s: %s",
                      app.config_path, error->message);
        }
        g_clear_error(&error);
    }

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Easy-Term");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1000, 640);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.window), "easy-term");

    gchar *binary_dir = g_path_get_dirname(argv[0]);
    gchar *logo_path = g_build_filename(binary_dir, "logo.png", NULL);
    if (g_file_test(logo_path, G_FILE_TEST_IS_REGULAR))
        gtk_window_set_icon_from_file(GTK_WINDOW(app.window), logo_path, NULL);
    g_free(logo_path);
    g_free(binary_dir);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(toolbar, 10);
    gtk_widget_set_margin_end(toolbar, 10);
    gtk_widget_set_margin_top(toolbar, 6);
    gtk_widget_set_margin_bottom(toolbar, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar),
                                "terminal-toolbar");
    GtkWidget *title = gtk_label_new("Easy-Term");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(title, TRUE);
    GtkWidget *add = gtk_button_new_from_icon_name("tab-new-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(add, "New terminal tab (Ctrl+Shift+T)");
    GtkWidget *preferences = gtk_button_new_from_icon_name("preferences-system-symbolic",
                                                            GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(preferences, "Show or hide terminal settings (F10)");
    gtk_box_pack_start(GTK_BOX(toolbar), title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), preferences, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), toolbar, FALSE, FALSE, 0);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app.notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(app.notebook), FALSE);
    gtk_box_pack_start(GTK_BOX(body), app.notebook, TRUE, TRUE, 0);
    app.settings_panel = create_settings_panel(&app);
    gtk_box_pack_start(GTK_BOX(body), app.settings_panel, FALSE, FALSE, 0);
    gtk_widget_hide(app.settings_panel);
    app.status = gtk_label_new("Ready");
    gtk_widget_set_halign(app.status, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.status), "status-line");
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), app.status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app.window), outer);

    load_settings_ui(&app);
    g_signal_connect(add, "clicked", G_CALLBACK(new_tab), &app);
    g_signal_connect(preferences, "clicked", G_CALLBACK(toggle_settings), &app);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.font_button, "font-set", G_CALLBACK(on_font_set), &app);
    g_signal_connect(app.font_size, "value-changed", G_CALLBACK(on_preference_changed), &app);
    g_signal_connect(app.foreground, "color-set", G_CALLBACK(on_preference_changed), &app);
    g_signal_connect(app.background, "color-set", G_CALLBACK(on_preference_changed), &app);
    g_signal_connect(app.blink, "notify::active", G_CALLBACK(on_switch_changed), &app);
    g_signal_connect(app.shell, "changed", G_CALLBACK(on_preference_changed), &app);
    g_signal_connect(app.scrollback, "value-changed", G_CALLBACK(on_preference_changed), &app);
    g_signal_connect(app.opacity, "value-changed", G_CALLBACK(on_preference_changed), &app);
    g_signal_connect(app.theme, "changed", G_CALLBACK(on_preference_changed), &app);

    g_signal_connect(app.reset_button, "clicked", G_CALLBACK(restore_defaults), &app);

    apply_preferences(&app);
    new_tab(&app);
    gtk_widget_show_all(app.window);
    gtk_widget_hide(app.settings_panel);
    gtk_main();

    dlclose(app.vte.handle);
    g_key_file_unref(app.config);
    g_free(app.config_path);
    return 0;
}
