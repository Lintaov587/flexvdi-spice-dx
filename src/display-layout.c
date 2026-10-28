/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "display-layout.h"
#include <json-glib/json-glib.h>
#include <string.h>

struct _DisplayLayout {
    GObject parent;
    SpiceSession *session;
    SpicePortChannel *port;
    GString *incoming;
    GArray *monitors;
    GtkWidget *dialog, *status, *list, *add, *editor, *width, *height, *automatic;
    gint editing_id, pending_width[4], pending_height[4];
    guint sequence, inflight, timeout, resize_timer;
    gboolean opened, reveal;
};
G_DEFINE_TYPE(DisplayLayout, display_layout, G_TYPE_OBJECT)
static guint changed_signal;
static void refresh_list(DisplayLayout *self);
static void queue_resize(DisplayLayout *self);

static void display_layout_dispose(GObject *object) {
    DisplayLayout *self = DISPLAY_LAYOUT(object);
    if (self->timeout) { g_source_remove(self->timeout); self->timeout = 0; }
    if (self->resize_timer) { g_source_remove(self->resize_timer); self->resize_timer = 0; }
    if (self->dialog) gtk_widget_destroy(self->dialog);
    g_clear_object(&self->port);
    g_clear_object(&self->session);
    G_OBJECT_CLASS(display_layout_parent_class)->dispose(object);
}
static void display_layout_finalize(GObject *object) {
    DisplayLayout *self = DISPLAY_LAYOUT(object);
    g_string_free(self->incoming, TRUE);
    g_array_unref(self->monitors);
    G_OBJECT_CLASS(display_layout_parent_class)->finalize(object);
}
static void display_layout_class_init(DisplayLayoutClass *klass) {
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->dispose = display_layout_dispose;
    object->finalize = display_layout_finalize;
    changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void display_layout_init(DisplayLayout *self) {
    self->incoming = g_string_new(NULL);
    self->monitors = g_array_new(FALSE, TRUE, sizeof(RemoteMonitor));
}
static void set_status(DisplayLayout *self, const gchar *message) {
    if (self->status) gtk_label_set_text(GTK_LABEL(self->status), message);
    g_message("Displays: %s", message);
}
static const RemoteMonitor *find_monitor(DisplayLayout *self, gint id) {
    for (guint i = 0; i < self->monitors->len; i++) {
        const RemoteMonitor *m = &g_array_index(self->monitors, RemoteMonitor, i);
        if (m->id == id) return m;
    }
    return NULL;
}
static void complete_request(DisplayLayout *self) {
    self->inflight = 0;
    if (self->timeout) { g_source_remove(self->timeout); self->timeout = 0; }
}
static gboolean request_timeout(gpointer data) {
    DisplayLayout *self = data;
    self->timeout = 0;
    self->inflight = 0;
    set_status(self, "显示配置响应超时，请检查远程显示服务。");
    refresh_list(self);
    return G_SOURCE_REMOVE;
}
static void write_done(GObject *port, GAsyncResult *result, gpointer data) {
    GError *error = NULL;
    if (spice_port_channel_write_finish(SPICE_PORT_CHANNEL(port), result, &error) < 0) {
        g_warning("Display configuration write failed: %s", error->message);
        g_clear_error(&error);
    }
    g_free(data);
}
static gboolean send_request(DisplayLayout *self, const gchar *body) {
    if (!self->opened || !self->port) {
        set_status(self, "远程显示服务尚未连接。");
        return FALSE;
    }
    if (self->inflight) return FALSE;
    self->sequence = self->sequence % 2147483646 + 1;
    self->inflight = self->sequence;
    gchar *request = g_strdup_printf("{\"version\":2,\"request_id\":%u,%s}\n", self->sequence, body);
    spice_port_channel_write_async(self->port, request, strlen(request), NULL, write_done, request);
    self->timeout = g_timeout_add_seconds(12, request_timeout, self);
    refresh_list(self);
    return TRUE;
}
static gboolean integer_member(JsonObject *object, const gchar *name, gint *value) {
    JsonNode *node = json_object_get_member(object, name);
    if (!node || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
    gint64 number = json_node_get_int(node);
    if (number < 0 || number > 8192) return FALSE;
    *value = number;
    return TRUE;
}
static void receive_line(DisplayLayout *self, const gchar *line) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, NULL)) return;
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) return;
    JsonObject *object = json_node_get_object(root);
    if (json_object_get_int_member_with_default(object, "version", 0) != 2) {
        set_status(self, "远程显示服务需要更新。");
        return;
    }
    if (self->inflight && json_object_get_int_member_with_default(object, "request_id", 0) == self->inflight)
        complete_request(self);
    const gchar *type = json_object_get_string_member_with_default(object, "type", "");
    if (g_str_equal(type, "error")) {
        set_status(self, json_object_get_string_member_with_default(object, "message", "显示配置失败"));
        refresh_list(self);
        queue_resize(self);
        return;
    }
    if (!g_str_equal(type, "state") || !json_object_has_member(object, "monitors")) return;
    JsonNode *node = json_object_get_member(object, "monitors");
    if (!JSON_NODE_HOLDS_ARRAY(node)) return;
    JsonArray *array = json_node_get_array(node);
    guint count = json_array_get_length(array), ids = 0;
    if (!count || count > 4) return;
    GArray *monitors = g_array_new(FALSE, TRUE, sizeof(RemoteMonitor));
    for (guint i = 0; i < count; i++) {
        node = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
        JsonObject *m = json_node_get_object(node);
        RemoteMonitor monitor = {0};
        if (!integer_member(m, "id", &monitor.id) || monitor.id > 3 || (ids & (1 << monitor.id)) ||
            !integer_member(m, "x", &monitor.x) || !integer_member(m, "y", &monitor.y) ||
            !integer_member(m, "width", &monitor.width) || !integer_member(m, "height", &monitor.height) ||
            !integer_member(m, "input_id", &monitor.input_id) || monitor.input_id > 255 ||
            monitor.width < 1 || monitor.height < 1 || monitor.x + monitor.width > 8192 ||
            monitor.y + monitor.height > 8192) goto invalid;
        JsonNode *automatic = json_object_get_member(m, "auto_resize");
        if (!automatic || json_node_get_value_type(automatic) != G_TYPE_BOOLEAN) goto invalid;
        monitor.auto_resize = json_node_get_boolean(automatic);
        ids |= 1 << monitor.id;
        g_array_append_val(monitors, monitor);
    }
    gboolean changed = monitors->len != self->monitors->len ||
        memcmp(monitors->data, self->monitors->data, monitors->len * sizeof(RemoteMonitor));
    g_array_unref(self->monitors);
    self->monitors = monitors;
    for (gint id = 0; id < 4; id++) {
        const RemoteMonitor *m = find_monitor(self, id);
        if (!m || !m->auto_resize ||
            (m->width == self->pending_width[id] && m->height == self->pending_height[id]))
            self->pending_width[id] = self->pending_height[id] = 0;
    }
    gchar *message = g_strdup_printf("已连接 %u 块显示器。", count);
    set_status(self, message);
    g_free(message);
    refresh_list(self);
    if (changed) g_signal_emit(self, changed_signal, 0);
    queue_resize(self);
    return;
invalid:
    g_array_unref(monitors);
}
static void port_data(SpicePortChannel *port, gpointer data, gint size, DisplayLayout *self) {
    if (size <= 0 || size > 8192 || self->incoming->len + size > 16384) {
        g_string_truncate(self->incoming, 0);
        return;
    }
    g_string_append_len(self->incoming, data, size);
    gchar *end;
    while ((end = strchr(self->incoming->str, '\n'))) {
        gsize length = end - self->incoming->str;
        gchar *line = g_strndup(self->incoming->str, length);
        g_string_erase(self->incoming, 0, length + 1);
        receive_line(self, line);
        g_free(line);
    }
}
static void port_opened(SpiceChannel *port, GParamSpec *spec, DisplayLayout *self) {
    g_object_get(port, "port-opened", &self->opened, NULL);
    g_string_truncate(self->incoming, 0);
    complete_request(self);
    if (self->opened) send_request(self, "\"op\":\"query\"");
    else { set_status(self, "远程显示服务已断开。"); refresh_list(self); }
}
static void port_name(SpiceChannel *port, GParamSpec *spec, DisplayLayout *self) {
    gchar *name = NULL;
    g_object_get(port, "port-name", &name, NULL);
    if (g_strcmp0(name, "org.spice-space.displays.0") == 0 && !self->port) {
        self->port = g_object_ref(SPICE_PORT_CHANNEL(port));
        g_signal_connect_object(port, "port-data", G_CALLBACK(port_data), self, 0);
        g_signal_connect_object(port, "notify::port-opened", G_CALLBACK(port_opened), self, 0);
        port_opened(port, NULL, self);
    }
    g_free(name);
}
static void channel_new(SpiceSession *session, SpiceChannel *channel, DisplayLayout *self) {
    if (!SPICE_IS_PORT_CHANNEL(channel)) return;
    g_signal_connect_object(channel, "notify::port-name", G_CALLBACK(port_name), self, 0);
    port_name(channel, NULL, self);
}
DisplayLayout *display_layout_new(ClientConn *conn) {
    DisplayLayout *self = g_object_new(DISPLAY_TYPE_LAYOUT, NULL);
    self->session = g_object_ref(client_conn_get_session(conn));
    g_signal_connect_object(self->session, "channel-new", G_CALLBACK(channel_new), self, 0);
    return self;
}
const GArray *display_layout_get_monitors(DisplayLayout *self) { return self ? self->monitors : NULL; }
gboolean display_layout_should_reveal(DisplayLayout *self) { return self && self->reveal; }

static gboolean resize_pending(gpointer data) {
    DisplayLayout *self = data;
    if (self->inflight || self->dialog) return G_SOURCE_CONTINUE;
    self->resize_timer = 0;
    for (gint id = 0; id < 4; id++) {
        const RemoteMonitor *m = find_monitor(self, id);
        gint w = self->pending_width[id], h = self->pending_height[id];
        if (!w) continue;
        self->pending_width[id] = self->pending_height[id] = 0;
        if (!m || !m->auto_resize || (w == m->width && h == m->height)) continue;
        gchar *body = g_strdup_printf("\"op\":\"resize\",\"id\":%d,\"width\":%d,\"height\":%d", id, w, h);
        send_request(self, body);
        g_free(body);
        break;
    }
    queue_resize(self);
    return G_SOURCE_REMOVE;
}
static void queue_resize(DisplayLayout *self) {
    if (self->resize_timer || !self->opened) return;
    for (gint id = 0; id < 4; id++) if (self->pending_width[id]) {
        self->resize_timer = g_timeout_add(400, resize_pending, self);
        break;
    }
}
void display_layout_resize(DisplayLayout *self, gint id, gint width, gint height) {
    if (!self || id < 0 || id > 3 || width < 320 || height < 200) return;
    const RemoteMonitor *m = find_monitor(self, id);
    if (!m || !m->auto_resize) return;
    self->pending_width[id] = (MIN(width, 3840) + 1) & ~1;
    self->pending_height[id] = (MIN(height, 2160) + 1) & ~1;
    if (self->resize_timer) { g_source_remove(self->resize_timer); self->resize_timer = 0; }
    queue_resize(self);
}
static void append_monitor(GString *body, gint id, gint w, gint h, gboolean automatic, gboolean *comma) {
    g_string_append_printf(body, "%s{\"id\":%d,\"width\":%d,\"height\":%d,\"auto_resize\":%s}",
                           *comma ? "," : "", id, w, h, automatic ? "true" : "false");
    *comma = TRUE;
}
static void edit_configuration(DisplayLayout *self, gint id, gint w, gint h, gboolean automatic, gboolean remove) {
    if (self->inflight) return;
    GString *body = g_string_new("\"op\":\"configure\",\"monitors\":[");
    gboolean comma = FALSE;
    for (guint i = 0; i < self->monitors->len; i++) {
        const RemoteMonitor *m = &g_array_index(self->monitors, RemoteMonitor, i);
        if (m->id != id) append_monitor(body, m->id, m->width, m->height, m->auto_resize, &comma);
    }
    if (!remove) append_monitor(body, id, w, h, automatic, &comma);
    g_string_append_c(body, ']');
    memset(self->pending_width, 0, sizeof(self->pending_width));
    memset(self->pending_height, 0, sizeof(self->pending_height));
    set_status(self, "正在应用显示器设置…");
    send_request(self, body->str);
    g_string_free(body, TRUE);
}
static void editor_destroy(GtkWidget *editor, DisplayLayout *self) {
    self->editor = self->width = self->height = self->automatic = NULL;
}
static void editor_response(GtkDialog *editor, gint response, DisplayLayout *self) {
    if (response == GTK_RESPONSE_APPLY) {
        gtk_spin_button_update(GTK_SPIN_BUTTON(self->width));
        gtk_spin_button_update(GTK_SPIN_BUTTON(self->height));
        edit_configuration(self, self->editing_id,
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->width)),
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->height)),
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->automatic)), FALSE);
    }
    gtk_widget_destroy(GTK_WIDGET(editor));
}
static void open_editor(DisplayLayout *self, gint id) {
    if (self->editor || self->inflight || !self->dialog) return;
    const RemoteMonitor *m = find_monitor(self, id);
    self->editing_id = id;
    self->editor = gtk_dialog_new_with_buttons(m ? "修改显示器" : "添加显示器", GTK_WINDOW(self->dialog),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "取消", GTK_RESPONSE_CANCEL,
        m ? "应用修改" : "添加", GTK_RESPONSE_APPLY, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(self->editor));
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_box_set_spacing(GTK_BOX(box), 14);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("分辨率"), FALSE, FALSE, 0);
    self->width = gtk_spin_button_new_with_range(320, 3840, 2);
    self->height = gtk_spin_button_new_with_range(200, 2160, 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->width), m ? m->width : 1280);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->height), m ? m->height : 720);
    gtk_box_pack_start(GTK_BOX(row), self->width, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("×"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), self->height, FALSE, FALSE, 0);
    self->automatic = gtk_check_button_new_with_label("拉伸窗口时自动调整此屏幕分辨率");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->automatic), m ? m->auto_resize : TRUE);
    gtk_box_pack_start(GTK_BOX(box), self->automatic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("支持自定义尺寸，按 2 像素对齐。"), FALSE, FALSE, 0);
    g_signal_connect(self->editor, "destroy", G_CALLBACK(editor_destroy), self);
    g_signal_connect(self->editor, "response", G_CALLBACK(editor_response), self);
    gtk_widget_show_all(self->editor);
}
static void edit_clicked(GtkButton *button, DisplayLayout *self) {
    open_editor(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "monitor-id")));
}
static void remove_clicked(GtkButton *button, DisplayLayout *self) {
    if (self->monitors->len <= 1 || self->inflight) return;
    edit_configuration(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "monitor-id")), 0, 0, FALSE, TRUE);
}
static void add_clicked(GtkButton *button, DisplayLayout *self) {
    for (gint id = 0; id < 4; id++) if (!find_monitor(self, id)) { open_editor(self, id); break; }
}
static void refresh_list(DisplayLayout *self) {
    if (!self->list) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(self->list));
    for (GList *p = children; p; p = p->next) gtk_widget_destroy(p->data);
    g_list_free(children);
    for (guint i = 0; i < self->monitors->len; i++) {
        const RemoteMonitor *m = &g_array_index(self->monitors, RemoteMonitor, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(row), 12);
        gchar *text = g_strdup_printf("显示器 %d%s\n%d × %d  ·  %s", m->id + 1,
            i == 0 ? "（主屏）" : "", m->width, m->height,
            m->auto_resize ? "随窗口调整" : "固定分辨率");
        GtkWidget *label = gtk_label_new(text);
        g_free(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
        GtkWidget *edit = gtk_button_new_with_label("修改");
        GtkWidget *remove = gtk_button_new_with_label("删除");
        g_object_set_data(G_OBJECT(edit), "monitor-id", GINT_TO_POINTER(m->id));
        g_object_set_data(G_OBJECT(remove), "monitor-id", GINT_TO_POINTER(m->id));
        g_signal_connect(edit, "clicked", G_CALLBACK(edit_clicked), self);
        g_signal_connect(remove, "clicked", G_CALLBACK(remove_clicked), self);
        gtk_widget_set_sensitive(edit, self->opened && !self->inflight);
        gtk_widget_set_sensitive(remove, self->opened && !self->inflight && self->monitors->len > 1);
        gtk_box_pack_start(GTK_BOX(row), edit, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(self->list), row);
    }
    gtk_widget_set_sensitive(self->add, self->opened && !self->inflight && self->monitors->len < 4);
    gtk_widget_show_all(self->list);
}
static void dialog_destroy(GtkWidget *dialog, DisplayLayout *self) {
    self->dialog = self->status = self->list = self->add = NULL;
}
static void dialog_response(GtkDialog *dialog, gint response, DisplayLayout *self) {
    if (response == 1) {
        self->reveal = TRUE;
        g_signal_emit(self, changed_signal, 0);
        self->reveal = FALSE;
    } else gtk_widget_destroy(GTK_WIDGET(dialog));
}
void display_layout_show(DisplayLayout *self, GtkWindow *parent) {
    if (self->dialog) { gtk_window_present(GTK_WINDOW(self->dialog)); return; }
    self->dialog = gtk_dialog_new_with_buttons("远程显示器", parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
        "显示所有窗口", 1, "关闭", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(self->dialog), 570, 340);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(self->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("每块显示器使用独立窗口，可分别设置分辨率。"), FALSE, FALSE, 0);
    self->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(box), self->list, TRUE, TRUE, 0);
    self->add = gtk_button_new_with_label("＋ 添加显示器");
    gtk_widget_set_halign(self->add, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), self->add, FALSE, FALSE, 0);
    g_signal_connect(self->add, "clicked", G_CALLBACK(add_clicked), self);
    self->status = gtk_label_new(self->opened ? "最多支持 4 块显示器。" : "等待远程显示服务连接…");
    gtk_label_set_line_wrap(GTK_LABEL(self->status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), self->status, FALSE, FALSE, 0);
    g_signal_connect(self->dialog, "destroy", G_CALLBACK(dialog_destroy), self);
    g_signal_connect(self->dialog, "response", G_CALLBACK(dialog_response), self);
    refresh_list(self);
    gtk_widget_show_all(self->dialog);
}
