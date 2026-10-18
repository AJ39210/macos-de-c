#include <gio/gio.h>
#include <gtk/gtk.h>
#include <string.h>

#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"
#define NOTIFICATIONS_INTERFACE "org.freedesktop.Notifications"
#define NOTIFICATIONS_NAME "org.freedesktop.Notifications"
#define DEFAULT_TIMEOUT 5000
#define CARD_WIDTH 380
#define CARD_GAP 12
#define SCREEN_MARGIN 18
#define TOPBAR_CLEARANCE 42

typedef struct {
    guint id;
    gchar *app_name;
    gchar *app_icon;
    gchar *summary;
    gchar *body;
    gchar **actions;
    gchar *time_text;
    GtkWidget *card;
    GtkWidget *history_row;
    guint timeout_id;
} Notification;

typedef struct {
    gchar *key;
    Notification *notification;
} ActionContext;

static GDBusConnection *bus_connection;
static GtkWidget *history_window;
static GtkWidget *history_list;
static GList *notifications;
static GList *active_cards;
static guint next_notification_id = 1;
static gboolean daemon_mode;
static gboolean service_acquired;
static gchar *application_directory;

static const gchar introspection_xml[] =
    "<node>"
    " <interface name='org.freedesktop.Notifications'>"
    "  <method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "  <method name='GetServerInformation'>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "  </method>"
    "  <method name='Notify'>"
    "   <arg type='s' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='s' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='u' direction='out'/>"
    "  </method>"
    "  <method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "  <signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "  <signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    " </interface>"
    "</node>";

static GDBusNodeInfo *introspection_data;

static void reposition_cards(void);
static void close_notification(Notification *notification, guint reason);

static void show_error(const gchar *title, const gchar *message)
{
    g_printerr("Notification Manager: %s: %s\n", title, message);
    GtkWidget *dialog = gtk_message_dialog_new(
        history_window ? GTK_WINDOW(history_window) : NULL,
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                              "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static GtkWidget *create_notification_image(const gchar *icon, gint size)
{
    GtkWidget *image = NULL;
    if (icon && *icon) {
        if (g_file_test(icon, G_FILE_TEST_IS_REGULAR)) {
            GError *error = NULL;
            GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
                icon, size, size, TRUE, &error);
            if (pixbuf) {
                image = gtk_image_new_from_pixbuf(pixbuf);
                g_object_unref(pixbuf);
            }
            g_clear_error(&error);
        } else {
            GtkIconTheme *theme = gtk_icon_theme_get_default();
            if (theme && gtk_icon_theme_has_icon(theme, icon))
                image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
        }
    }
    if (!image) {
        gchar *logo_path = g_build_filename(application_directory, "logo.png", NULL);
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
            logo_path, size, size, TRUE, NULL);
        if (pixbuf) {
            image = gtk_image_new_from_pixbuf(pixbuf);
            g_object_unref(pixbuf);
        } else {
            image = gtk_image_new_from_icon_name("dialog-information",
                                                 GTK_ICON_SIZE_DIALOG);
        }
        g_free(logo_path);
    }
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    return image;
}

static void free_action_context(gpointer data, GClosure *closure)
{
    (void)closure;
    ActionContext *context = data;
    g_free(context->key);
    g_free(context);
}

static void emit_action(Notification *notification, const gchar *key)
{
    if (!bus_connection)
        return;
    GError *error = NULL;
    g_dbus_connection_emit_signal(bus_connection, NULL, NOTIFICATIONS_PATH,
                                  NOTIFICATIONS_INTERFACE, "ActionInvoked",
                                  g_variant_new("(us)", notification->id, key),
                                  &error);
    if (error) {
        g_warning("Could not emit ActionInvoked: %s", error->message);
        g_error_free(error);
    }
}

static void action_button_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    ActionContext *context = user_data;
    emit_action(context->notification, context->key);
    close_notification(context->notification, 2);
}

static void card_default_action(GtkButton *button, gpointer user_data)
{
    (void)button;
    Notification *notification = user_data;
    if (notification->actions && notification->actions[0])
        emit_action(notification, "default");
    close_notification(notification, 2);
}

static gboolean card_delete_event(GtkWidget *widget, GdkEvent *event,
                                  gpointer user_data)
{
    (void)widget;
    (void)event;
    close_notification(user_data, 2);
    return TRUE;
}

static gboolean clear_notification_window(GtkWidget *widget, cairo_t *cr,
                                          gpointer user_data)
{
    (void)widget;
    (void)user_data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static void reposition_cards(void)
{
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = display ? gdk_display_get_primary_monitor(display) : NULL;
    GdkRectangle geometry = { 0, 0, 1280, 800 };
    if (monitor)
        gdk_monitor_get_geometry(monitor, &geometry);

    gint y = geometry.y + TOPBAR_CLEARANCE;
    for (GList *item = active_cards; item; item = item->next) {
        Notification *notification = item->data;
        if (!notification->card)
            continue;
        gint width = CARD_WIDTH;
        gint height = gtk_widget_get_allocated_height(notification->card);
        if (height < 80) {
            GtkRequisition minimum;
            GtkRequisition natural;
            gtk_widget_get_preferred_size(notification->card, &minimum,
                                          &natural);
            height = MAX(minimum.height, natural.height);
        }
        gint x = geometry.x + geometry.width - width - SCREEN_MARGIN;
        gtk_window_move(GTK_WINDOW(notification->card), x, y);
        y += height + CARD_GAP;
    }
}

static void destroy_card(Notification *notification)
{
    if (notification->timeout_id) {
        g_source_remove(notification->timeout_id);
        notification->timeout_id = 0;
    }
    if (notification->card) {
        GtkWidget *card = notification->card;
        notification->card = NULL;
        gtk_widget_destroy(card);
    }
    active_cards = g_list_remove(active_cards, notification);
    reposition_cards();
}

static void close_notification(Notification *notification, guint reason)
{
    if (!notification || !notification->card)
        return;
    guint id = notification->id;
    destroy_card(notification);
    if (bus_connection) {
        GError *error = NULL;
        g_dbus_connection_emit_signal(bus_connection, NULL, NOTIFICATIONS_PATH,
                                      NOTIFICATIONS_INTERFACE,
                                      "NotificationClosed",
                                      g_variant_new("(uu)", id, reason), &error);
        if (error) {
            g_warning("Could not emit NotificationClosed: %s", error->message);
            g_error_free(error);
        }
    }
}

static gboolean notification_expired(gpointer user_data)
{
    Notification *notification = user_data;
    notification->timeout_id = 0;
    close_notification(notification, 1);
    return G_SOURCE_REMOVE;
}

static GtkWidget *create_history_row(Notification *notification)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *image = create_notification_image(notification->app_icon, 42);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *app = gtk_label_new(notification->app_name);
    GtkWidget *time = gtk_label_new(notification->time_text);
    GtkWidget *summary = gtk_label_new(notification->summary);
    GtkWidget *body = gtk_label_new(notification->body);

    gtk_style_context_add_class(gtk_widget_get_style_context(row), "history-row");
    gtk_style_context_add_class(gtk_widget_get_style_context(app), "history-app");
    gtk_style_context_add_class(gtk_widget_get_style_context(time), "history-time");
    gtk_style_context_add_class(gtk_widget_get_style_context(summary), "history-summary");
    gtk_style_context_add_class(gtk_widget_get_style_context(body), "history-body");
    gtk_widget_set_halign(app, GTK_ALIGN_START);
    gtk_widget_set_halign(time, GTK_ALIGN_END);
    gtk_widget_set_halign(summary, GTK_ALIGN_START);
    gtk_widget_set_halign(body, GTK_ALIGN_START);
    gtk_widget_set_hexpand(time, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
    gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(body), 72);
    gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
    gtk_box_pack_start(GTK_BOX(heading), app, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(heading), time, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), heading, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), summary, TRUE, TRUE, 0);
    if (notification->body && *notification->body)
        gtk_box_pack_start(GTK_BOX(content), body, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), content, TRUE, TRUE, 0);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 13);
    gtk_widget_set_margin_bottom(box, 13);
    gtk_container_add(GTK_CONTAINER(row), box);
    return row;
}

static GtkWidget *create_card(Notification *notification)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *card = gtk_grid_new();
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *image = create_notification_image(notification->app_icon, 38);
    GtkWidget *app = gtk_label_new(notification->app_name);
    GtkWidget *time = gtk_label_new(notification->time_text);
    GtkWidget *dismiss = gtk_button_new_from_icon_name("window-close-symbolic",
                                                       GTK_ICON_SIZE_MENU);
    GtkWidget *title = gtk_label_new(notification->summary);
    GtkWidget *body = gtk_label_new(notification->body);

    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_window_set_default_size(GTK_WINDOW(window), CARD_WIDTH, -1);
    gtk_widget_set_size_request(card, CARD_WIDTH - 30, -1);
    gtk_widget_set_app_paintable(window, TRUE);
    GdkScreen *screen = gtk_widget_get_screen(window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual)
        gtk_widget_set_visual(window, visual);
    g_signal_connect(window, "draw",
                     G_CALLBACK(clear_notification_window), NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "notification-window");
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "notification-card");
    gtk_style_context_add_class(gtk_widget_get_style_context(header),
                                "notification-header");
    gtk_style_context_add_class(gtk_widget_get_style_context(app), "notification-app");
    gtk_style_context_add_class(gtk_widget_get_style_context(time), "notification-time");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "notification-title");
    gtk_style_context_add_class(gtk_widget_get_style_context(body), "notification-body");
    gtk_style_context_add_class(gtk_widget_get_style_context(dismiss),
                                "notification-dismiss");
    gtk_widget_set_halign(app, GTK_ALIGN_START);
    gtk_widget_set_hexpand(app, TRUE);
    gtk_widget_set_halign(time, GTK_ALIGN_END);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_halign(body, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(app), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app), 30);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 46);
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(body), 46);
    gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
    gtk_button_set_relief(GTK_BUTTON(dismiss), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(dismiss, 28, 28);
    gtk_widget_set_tooltip_text(dismiss, "Dismiss notification");
    gtk_widget_set_valign(image, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(header), app, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(header), time, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), dismiss, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 0);
    if (notification->body && *notification->body)
        gtk_box_pack_start(GTK_BOX(content), body, FALSE, FALSE, 0);
    gtk_grid_set_column_spacing(GTK_GRID(card), 12);
    gtk_grid_attach(GTK_GRID(card), image, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(card), content, 1, 0, 1, 1);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_margin_start(card, 15);
    gtk_widget_set_margin_end(card, 15);
    gtk_widget_set_margin_top(card, 13);
    gtk_widget_set_margin_bottom(card, 13);

    if (notification->actions) {
        GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        for (guint i = 0; notification->actions[i] && notification->actions[i + 1]; i += 2) {
            GtkWidget *button = gtk_button_new_with_label(notification->actions[i + 1]);
            ActionContext *context = g_new0(ActionContext, 1);
            context->key = g_strdup(notification->actions[i]);
            context->notification = notification;
            g_signal_connect_data(button, "clicked", G_CALLBACK(action_button_clicked),
                                  context, free_action_context, 0);
            gtk_box_pack_end(GTK_BOX(action_box), button, FALSE, FALSE, 0);
        }
        GList *action_children = gtk_container_get_children(GTK_CONTAINER(action_box));
        if (action_children) {
            gtk_widget_set_hexpand(action_box, TRUE);
            gtk_grid_attach(GTK_GRID(card), action_box, 0, 1, 2, 1);
        } else {
            gtk_widget_destroy(action_box);
        }
        g_list_free(action_children);
    }
    gtk_container_add(GTK_CONTAINER(window), card);
    g_signal_connect(dismiss, "clicked", G_CALLBACK(card_default_action), notification);
    g_signal_connect(window, "delete-event", G_CALLBACK(card_delete_event), notification);
    return window;
}

static Notification *find_notification(guint id)
{
    for (GList *item = notifications; item; item = item->next) {
        Notification *notification = item->data;
        if (notification->id == id)
            return notification;
    }
    return NULL;
}

static Notification *find_active_notification(guint id)
{
    Notification *notification = find_notification(id);
    return notification && notification->card ? notification : NULL;
}

static void add_notification(const gchar *app_name, guint replaces_id,
                             const gchar *app_icon, const gchar *summary,
                             const gchar *body, gchar **actions,
                             gint expire_timeout, guint *result_id)
{
    Notification *notification = replaces_id ? find_active_notification(replaces_id) : NULL;
    if (notification && notification->card) {
        destroy_card(notification);
        if (bus_connection) {
            GError *error = NULL;
            g_dbus_connection_emit_signal(bus_connection, NULL, NOTIFICATIONS_PATH,
                                          NOTIFICATIONS_INTERFACE,
                                          "NotificationClosed",
                                          g_variant_new("(uu)", notification->id, 3u),
                                          &error);
            if (error) {
                g_warning("Could not emit NotificationClosed: %s", error->message);
                g_error_free(error);
            }
        }
    }
    if (!notification) {
        notification = g_new0(Notification, 1);
        notification->id = next_notification_id++;
        if (next_notification_id == 0)
            next_notification_id = 1;
        notifications = g_list_prepend(notifications, notification);
    } else {
        g_free(notification->app_name);
        g_free(notification->app_icon);
        g_free(notification->summary);
        g_free(notification->body);
        g_strfreev(notification->actions);
        g_free(notification->time_text);
        if (notification->history_row) {
            gtk_widget_destroy(notification->history_row);
            notification->history_row = NULL;
        }
    }

    notification->app_name = g_strdup(app_name && *app_name ? app_name : "Notification");
    notification->app_icon = g_strdup(app_icon ? app_icon : "");
    notification->summary = g_strdup(summary ? summary : "");
    notification->body = g_strdup(body ? body : "");
    notification->actions = actions ? g_strdupv(actions) : NULL;
    GDateTime *now = g_date_time_new_now_local();
    notification->time_text = g_date_time_format(now, "%H:%M");
    g_date_time_unref(now);

    if (history_list) {
        notification->history_row = create_history_row(notification);
        gtk_list_box_insert(GTK_LIST_BOX(history_list), notification->history_row, 0);
        gtk_widget_show_all(notification->history_row);
    }

    notification->card = create_card(notification);
    active_cards = g_list_append(active_cards, notification);
    gtk_widget_show_all(notification->card);
    reposition_cards();

    if (expire_timeout < 0)
        expire_timeout = DEFAULT_TIMEOUT;
    if (expire_timeout > 0)
        notification->timeout_id =
            g_timeout_add((guint)expire_timeout, notification_expired, notification);
    *result_id = notification->id;
}

static void clear_history_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    GList *children = gtk_container_get_children(GTK_CONTAINER(history_list));
    for (GList *item = notifications; item; item = item->next) {
        Notification *notification = item->data;
        notification->history_row = NULL;
    }
    for (GList *item = children; item; item = item->next)
        gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
}

static void history_window_destroyed(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    (void)user_data;
    gtk_main_quit();
}

static void show_history_window(void)
{
    history_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(history_window), "Notification Manager");
    gtk_window_set_default_size(GTK_WINDOW(history_window), 640, 520);
    gtk_window_set_position(GTK_WINDOW(history_window), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name(GTK_WINDOW(history_window), "preferences-system-notifications");
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *heading = gtk_label_new("Notification History");
    GtkWidget *clear = gtk_button_new_with_label("Clear history");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    history_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(history_list), GTK_SELECTION_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(root), "history-root");
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "history-toolbar");
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "history-heading");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_widget_set_margin_start(toolbar, 20);
    gtk_widget_set_margin_end(toolbar, 20);
    gtk_widget_set_margin_top(toolbar, 16);
    gtk_widget_set_margin_bottom(toolbar, 16);
    gtk_container_set_border_width(GTK_CONTAINER(scroll), 10);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), history_list);
    gtk_box_pack_start(GTK_BOX(toolbar), heading, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(history_window), root);
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_history_clicked), NULL);
    g_signal_connect(history_window, "destroy",
                     G_CALLBACK(history_window_destroyed), NULL);
    gtk_widget_show_all(history_window);

    for (GList *item = g_list_last(notifications); item; item = item->prev) {
        Notification *notification = item->data;
        notification->history_row = create_history_row(notification);
        gtk_list_box_insert(GTK_LIST_BOX(history_list), notification->history_row, 0);
        gtk_widget_show_all(notification->history_row);
    }
}

static void handle_method_call(GDBusConnection *connection, const gchar *sender,
                               const gchar *object_path, const gchar *interface_name,
                               const gchar *method_name, GVariant *parameters,
                               GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_str_equal(method_name, "GetCapabilities")) {
        const gchar *capabilities[] = { "actions", "body", "icon-static", NULL };
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(^as)", capabilities));
    } else if (g_str_equal(method_name, "GetServerInformation")) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ssss)", "Notification Manager",
                                      "macOS-DE", "1.0.0", "1.2"));
    } else if (g_str_equal(method_name, "Notify")) {
        const gchar *app_name, *app_icon, *summary, *body;
        guint replaces_id;
        GVariant *actions_variant, *hints;
        gint expire_timeout;
        g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)", &app_name, &replaces_id,
                      &app_icon, &summary, &body, &actions_variant, &hints,
                      &expire_timeout);
        gchar **actions = g_variant_dup_strv(actions_variant, NULL);
        guint id;
        add_notification(app_name, replaces_id, app_icon, summary, body,
                         actions, expire_timeout, &id);
        g_strfreev(actions);
        g_variant_unref(actions_variant);
        g_variant_unref(hints);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
    } else if (g_str_equal(method_name, "CloseNotification")) {
        guint id;
        g_variant_get(parameters, "(u)", &id);
        Notification *notification = find_notification(id);
        if (notification && notification->card)
            close_notification(notification, 3);
        else {
            g_dbus_method_invocation_return_error(
                invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                "Notification ID %u is not active", id);
            return;
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    NULL,
    NULL,
    { 0 }
};

static void name_acquired(GDBusConnection *connection, const gchar *name,
                          gpointer user_data)
{
    (void)connection;
    (void)name;
    (void)user_data;
    service_acquired = TRUE;
    g_message("Notification Manager owns %s", NOTIFICATIONS_NAME);
}

static void name_lost(GDBusConnection *connection, const gchar *name,
                      gpointer user_data)
{
    (void)connection;
    (void)user_data;
    gchar *message = g_strdup_printf(
        "Could not acquire %s on the session bus. Another notification daemon may "
        "already be running, or the bus denied ownership.", name);
    show_error("Notification service unavailable", message);
    g_free(message);
    gtk_main_quit();
}

static gboolean register_notification_service(void)
{
    GError *error = NULL;
    bus_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus_connection) {
        show_error("Could not connect to the session bus",
                   error ? error->message : "No D-Bus session bus is available.");
        g_clear_error(&error);
        return FALSE;
    }

    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!introspection_data) {
        show_error("Could not initialize notification service", error->message);
        g_clear_error(&error);
        return FALSE;
    }
    guint registration_id = g_dbus_connection_register_object(
        bus_connection, NOTIFICATIONS_PATH,
        introspection_data->interfaces[0], &interface_vtable,
        NULL, NULL, &error);
    if (!registration_id) {
        show_error("Could not register notification interface", error->message);
        g_clear_error(&error);
        return FALSE;
    }
    g_bus_own_name_on_connection(bus_connection, NOTIFICATIONS_NAME,
                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                name_acquired, name_lost, NULL, NULL);
    return TRUE;
}

static void load_css(void)
{
    gchar *css_path = g_build_filename(application_directory,
                                       "Notification-manager.css", NULL);
    if (!g_file_test(css_path, G_FILE_TEST_IS_REGULAR)) {
        g_free(css_path);
        return;
    }
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_path(provider, css_path, &error)) {
        g_warning("Could not load notification stylesheet: %s", error->message);
        g_error_free(error);
    } else {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
    g_free(css_path);
}

static void free_notification(gpointer data)
{
    Notification *notification = data;
    if (notification->timeout_id)
        g_source_remove(notification->timeout_id);
    if (notification->card)
        gtk_widget_destroy(notification->card);
    g_free(notification->app_name);
    g_free(notification->app_icon);
    g_free(notification->summary);
    g_free(notification->body);
    g_strfreev(notification->actions);
    g_free(notification->time_text);
    g_free(notification);
}

int main(int argc, char **argv)
{
    gboolean history_only = FALSE;
    for (gint i = 1; i < argc; i++) {
        if (g_str_equal(argv[i], "--daemon")) {
            daemon_mode = TRUE;
        } else if (g_str_equal(argv[i], "--history")) {
            history_only = TRUE;
        } else if (g_str_equal(argv[i], "--help")) {
            g_print("Usage: %s [--daemon | --history]\n"
                    "  default      Open notification history and serve D-Bus notifications\n"
                    "  --daemon     Run the D-Bus notification service without the history window\n"
                    "  --history    Open the history window without owning the notification service\n",
                    argv[0]);
            return 0;
        } else {
            g_printerr("Unknown option: %s\n", argv[i]);
            return 2;
        }
    }
    if (daemon_mode && history_only) {
        g_printerr("Choose either --daemon or --history, not both.\n");
        return 2;
    }

    gchar *program_path = g_find_program_in_path(argv[0]);
    application_directory = g_path_get_dirname(program_path ? program_path : argv[0]);
    g_free(program_path);
    gtk_init(&argc, &argv);
    load_css();
    if (!daemon_mode)
        show_history_window();
    if (!history_only && !register_notification_service()) {
        g_list_free_full(notifications, free_notification);
        g_list_free(active_cards);
        g_clear_object(&bus_connection);
        if (introspection_data)
            g_dbus_node_info_unref(introspection_data);
        g_free(application_directory);
        return 1;
    }

    gtk_main();

    g_list_free(active_cards);
    active_cards = NULL;
    g_list_free_full(notifications, free_notification);
    g_clear_object(&bus_connection);
    if (introspection_data)
        g_dbus_node_info_unref(introspection_data);
    g_free(application_directory);
    return service_acquired || history_only ? 0 : 1;
}
