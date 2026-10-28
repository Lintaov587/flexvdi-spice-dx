/* Local connection history. SPDX-License-Identifier: GPL-3.0-or-later */
#include "connection-history.h"
#include <glib/gstdio.h>
#include <string.h>

#define HISTORY_LIMIT 50
#define PREVIEW_WIDTH 256
#define PREVIEW_HEIGHT 144

typedef struct {
    gchar *id, *uri, *name;
    gint64 connected_at;
    gboolean dx, low_latency;
} HistoryEntry;

static gchar *history_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "flexvdi-client", "history", NULL);
}

static gchar *entry_path(const gchar *id, const gchar *extension) {
    g_autofree gchar *dir = history_dir();
    g_autofree gchar *filename = g_strconcat(id, extension, NULL);
    return g_build_filename(dir, filename, NULL);
}

static void entry_free(gpointer data) {
    HistoryEntry *entry = data;
    g_free(entry->id); g_free(entry->uri); g_free(entry->name); g_free(entry);
}

static gboolean valid_port(const gchar *port) {
    if (!port || !*port) return FALSE;
    for (const gchar *p = port; *p; ++p) if (!g_ascii_isdigit(*p)) return FALSE;
    guint64 value = g_ascii_strtoull(port, NULL, 10);
    return value > 0 && value <= 65535;
}

static gchar *invalid_uri(GError **error) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "请输入有效的 SPICE 地址，例如 spice://127.0.0.1:5900");
    return NULL;
}

gchar *client_history_normalize_uri(const gchar *input, GError **error) {
    g_autofree gchar *trimmed = g_strdup(input ? input : "");
    g_strstrip(trimmed);
    g_autofree gchar *address = strstr(trimmed, "://") ? g_strdup(trimmed) :
                                g_strconcat("spice://", trimmed, NULL);
    g_autoptr(GUri) uri = g_uri_parse(address, G_URI_FLAGS_HAS_PASSWORD, NULL);
    if (!uri || g_strcmp0(g_uri_get_scheme(uri), "spice") ||
        !g_uri_get_host(uri) || !*g_uri_get_host(uri) ||
        (g_uri_get_path(uri) && *g_uri_get_path(uri) && strcmp(g_uri_get_path(uri), "/")))
        return invalid_uri(error);
    const gchar *query = g_uri_get_query(uri);
    g_autoptr(GHashTable) params = query ? g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
    if (query && !params) return invalid_uri(error);
    const gchar *tls = params ? g_hash_table_lookup(params, "tls-port") : NULL;
    const gchar *qport = params ? g_hash_table_lookup(params, "port") : NULL;
    gint port = g_uri_get_port(uri);
    if ((tls && !valid_port(tls)) || (qport && !valid_port(qport)) || port == 0 || port > 65535)
        return invalid_uri(error);
    if (qport) port = (gint)g_ascii_strtoull(qport, NULL, 10);
    if (port < 0 && !tls) port = 5900;
    g_autofree gchar *host = g_ascii_strdown(g_uri_get_host(uri), -1);
    g_autofree gchar *authority = strchr(host, ':') ? g_strdup_printf("[%s]", host) : g_strdup(host);
    g_autofree gchar *plain = port > 0 ? g_strdup_printf(":%d", port) : g_strdup("");
    g_autofree gchar *secure = tls ? g_strdup_printf("?tls-port=%u", (guint)g_ascii_strtoull(tls, NULL, 10)) : g_strdup("");
    /* Only endpoint fields are persisted: no userinfo, password, token or fragment. */
    return g_strdup_printf("spice://%s%s%s", authority, plain, secure);
}

static gint entry_compare(gconstpointer a, gconstpointer b) {
    const HistoryEntry *ea = *(HistoryEntry *const *)a, *eb = *(HistoryEntry *const *)b;
    return ea->connected_at < eb->connected_at ? 1 : ea->connected_at > eb->connected_at ? -1 : g_strcmp0(ea->id, eb->id);
}

static GPtrArray *history_load(void) {
    GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);
    g_autofree gchar *dir = history_dir();
    GDir *listing = g_dir_open(dir, 0, NULL);
    if (!listing) return entries;
    const gchar *filename;
    while ((filename = g_dir_read_name(listing))) {
        if (strlen(filename) != 68 || strcmp(filename + 64, ".ini")) continue;
        gboolean valid = TRUE;
        for (guint i = 0; i < 64; i++) if (!g_ascii_isxdigit(filename[i])) valid = FALSE;
        if (!valid) continue;
        g_autofree gchar *path = g_build_filename(dir, filename, NULL);
        g_autoptr(GKeyFile) key = g_key_file_new();
        if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL)) continue;
        g_autofree gchar *uri = g_key_file_get_string(key, "Connection", "uri", NULL);
        g_autofree gchar *canonical = client_history_normalize_uri(uri, NULL);
        if (!uri || !canonical || strcmp(uri, canonical)) continue;
        g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
        if (strncmp(filename, digest, 64)) continue;
        HistoryEntry *entry = g_new0(HistoryEntry, 1);
        entry->id = g_strdup(digest);
        entry->uri = g_strdup(uri);
        entry->name = g_key_file_get_string(key, "Connection", "name", NULL);
        if (!entry->name || !*entry->name) { g_free(entry->name); entry->name = g_strdup(uri); }
        entry->connected_at = g_key_file_get_int64(key, "Connection", "last-connected", NULL);
        entry->dx = g_key_file_get_boolean(key, "Connection", "dx", NULL);
        entry->low_latency = !g_key_file_has_key(key, "Connection", "low-latency", NULL) ||
                             g_key_file_get_boolean(key, "Connection", "low-latency", NULL);
        g_ptr_array_add(entries, entry);
    }
    g_dir_close(listing);
    g_ptr_array_sort(entries, entry_compare);
    return entries;
}

static gboolean history_remove(const gchar *id, GError **error) {
    g_autofree gchar *record = entry_path(id, ".ini");
    g_autofree gchar *image = entry_path(id, ".png");
    if (g_file_test(image, G_FILE_TEST_EXISTS) && g_unlink(image) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "无法删除桌面缩略图，请检查文件权限。");
        return FALSE;
    }
    if (g_unlink(record) != 0 && g_file_test(record, G_FILE_TEST_EXISTS)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "无法删除连接记录，请检查文件权限。");
        return FALSE;
    }
    return TRUE;
}

void client_history_record(const gchar *uri, const gchar *name, gboolean dx,
                           gint64 connected_at, GdkPixbuf *desktop, gboolean first) {
    g_autofree gchar *canonical = client_history_normalize_uri(uri, NULL);
    if (!canonical || !desktop) return;
    g_autofree gchar *id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical, -1);
    g_autofree gchar *record = entry_path(id, ".ini");
    /* A record deleted during an active connection must stay deleted. */
    if (!first && !g_file_test(record, G_FILE_TEST_EXISTS)) return;
    g_autofree gchar *dir = history_dir();
    if (g_mkdir_with_parents(dir, 0700) != 0) { g_warning("Cannot create connection history directory"); return; }
    g_autoptr(GKeyFile) key = g_key_file_new();
    g_key_file_load_from_file(key, record, G_KEY_FILE_NONE, NULL);
    if (g_key_file_get_int64(key, "Connection", "last-connected", NULL) > connected_at) return;
    g_autoptr(GdkPixbuf) preview = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, PREVIEW_WIDTH, PREVIEW_HEIGHT);
    gdk_pixbuf_fill(preview, 0x182334ff);
    gdouble scale = MIN((gdouble)PREVIEW_WIDTH / gdk_pixbuf_get_width(desktop),
                        (gdouble)PREVIEW_HEIGHT / gdk_pixbuf_get_height(desktop));
    gint width = MAX(1, (gint)(gdk_pixbuf_get_width(desktop) * scale));
    gint height = MAX(1, (gint)(gdk_pixbuf_get_height(desktop) * scale));
    gint x = (PREVIEW_WIDTH - width) / 2, y = (PREVIEW_HEIGHT - height) / 2;
    gdk_pixbuf_scale(desktop, preview, x, y, width, height, x, y, scale, scale, GDK_INTERP_BILINEAR);
    g_autofree gchar *png = NULL;
    g_autofree gchar *data = NULL;
    gsize size = 0;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *image = entry_path(id, ".png");
    if (!gdk_pixbuf_save_to_buffer(preview, &png, &size, "png", &error, NULL) ||
        !g_file_set_contents(image, png, size, &error)) goto failed;
    g_key_file_set_string(key, "Connection", "uri", canonical);
    g_key_file_set_string(key, "Connection", "name", name && *name ? name : canonical);
    g_key_file_set_int64(key, "Connection", "last-connected", connected_at);
    g_key_file_set_boolean(key, "Connection", "dx", dx);
    g_key_file_set_boolean(key, "Connection", "low-latency", g_strcmp0(g_getenv("SPICE_DX_LOW_LATENCY"), "0") != 0);
    data = g_key_file_to_data(key, &size, NULL);
    if (!g_file_set_contents(record, data, size, &error)) goto failed;
    if (first) {
        g_autoptr(GPtrArray) entries = history_load();
        for (guint i = HISTORY_LIMIT; i < entries->len; i++)
            history_remove(((HistoryEntry *)g_ptr_array_index(entries, i))->id, NULL);
        g_message("History: saved successful connection and desktop thumbnail");
    }
    return;
failed:
    g_warning("Cannot save connection history: %s", error ? error->message : "unknown error");
}

typedef struct {
    GtkWidget *root, *flow, *empty, *count, *entry, *dx, *low_latency, *fullscreen, *clear;
    ClientHistoryConnect connect;
    gpointer user_data;
} HistoryView;

static void refresh_view(HistoryView *view);

static void show_error(HistoryView *view, const gchar *text) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(view->root)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", text);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void connect_address(GtkWidget *source, HistoryView *view) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *uri = client_history_normalize_uri(gtk_entry_get_text(GTK_ENTRY(view->entry)), &error);
    if (!uri) { show_error(view, error->message); return; }
    view->connect(uri, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->dx)),
                  gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->low_latency)),
                  gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->fullscreen)), view->user_data);
}

static void connect_recent(GtkButton *button, HistoryView *view) {
    const gchar *uri = g_object_get_data(G_OBJECT(button), "uri");
    gboolean dx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "dx"));
    gtk_entry_set_text(GTK_ENTRY(view->entry), uri);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->dx), dx);
    gboolean low_latency = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "low-latency"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->low_latency), low_latency);
    view->connect(uri, dx, low_latency, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->fullscreen)), view->user_data);
}

static void delete_recent(GtkButton *button, HistoryView *view) {
    g_autofree gchar *id = g_strdup(g_object_get_data(G_OBJECT(button), "id"));
    g_autoptr(GError) error = NULL;
    if (!history_remove(id, &error)) show_error(view, error->message);
    refresh_view(view);
}

static void clear_history(GtkButton *button, HistoryView *view) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(view->root)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "清空所有连接历史和桌面缩略图？");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "取消", GTK_RESPONSE_CANCEL, "清空历史", GTK_RESPONSE_ACCEPT, NULL);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT) return;
    g_autoptr(GPtrArray) entries = history_load();
    for (guint i = 0; i < entries->len; i++) {
        g_autoptr(GError) error = NULL;
        if (!history_remove(((HistoryEntry *)g_ptr_array_index(entries, i))->id, &error)) {
            show_error(view, error->message); break;
        }
    }
    refresh_view(view);
}

static GtkWidget *text_label(const gchar *text, const gchar *css) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (css) gtk_style_context_add_class(gtk_widget_get_style_context(label), css);
    return label;
}

static void refresh_view(HistoryView *view) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->flow));
    for (GList *p = children; p; p = p->next) gtk_widget_destroy(p->data);
    g_list_free(children);
    g_autoptr(GPtrArray) entries = history_load();
    g_autofree gchar *count = g_strdup_printf("最近连接  ·  %u", entries->len);
    gtk_label_set_text(GTK_LABEL(view->count), count);
    gtk_widget_set_sensitive(view->clear, entries->len > 0);
    for (guint i = 0; i < entries->len; i++) {
        HistoryEntry *entry = g_ptr_array_index(entries, i);
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_halign(card, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "history-card");
        GtkWidget *button = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "history-connect");
        gtk_widget_set_tooltip_text(button, "点击连接这台桌面");
        g_object_set_data_full(G_OBJECT(button), "uri", g_strdup(entry->uri), g_free);
        g_object_set_data(G_OBJECT(button), "dx", GINT_TO_POINTER(entry->dx));
        g_object_set_data(G_OBJECT(button), "low-latency", GINT_TO_POINTER(entry->low_latency));
        g_signal_connect(button, "clicked", G_CALLBACK(connect_recent), view);
        GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        g_autofree gchar *path = entry_path(entry->id, ".png");
        g_autoptr(GdkPixbuf) preview = gdk_pixbuf_new_from_file_at_scale(path, PREVIEW_WIDTH, PREVIEW_HEIGHT, TRUE, NULL);
        GtkWidget *image = preview ? gtk_image_new_from_pixbuf(preview) : gtk_image_new_from_icon_name("computer-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_widget_set_size_request(image, PREVIEW_WIDTH, PREVIEW_HEIGHT);
        gtk_box_pack_start(GTK_BOX(body), image, FALSE, FALSE, 0);
        GtkWidget *title = text_label(entry->name, "history-name");
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(title), 24);
        gtk_widget_set_tooltip_text(title, entry->name);
        gtk_box_pack_start(GTK_BOX(body), title, FALSE, FALSE, 0);
        GtkWidget *address = text_label(entry->uri, "history-muted");
        gtk_label_set_ellipsize(GTK_LABEL(address), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(address), 28);
        gtk_widget_set_tooltip_text(address, entry->uri);
        gtk_box_pack_start(GTK_BOX(body), address, FALSE, FALSE, 0);
        g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(entry->connected_at);
        g_autoptr(GDateTime) local = utc ? g_date_time_to_local(utc) : NULL;
        g_autofree gchar *date = local ? g_date_time_format(local, "%m月%d日 %H:%M") : g_strdup("时间未知");
        g_autofree gchar *details = g_strdup_printf("%s  ·  %s", date,
            entry->dx ? (entry->low_latency ? "DX · 低延迟" : "DX · 音画同步") : "标准 SPICE");
        gtk_box_pack_start(GTK_BOX(body), text_label(details, "history-muted"), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), body);
        gtk_box_pack_start(GTK_BOX(card), button, TRUE, TRUE, 0);
        GtkWidget *remove = gtk_button_new_with_label("移除记录");
        gtk_widget_set_halign(remove, GTK_ALIGN_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(remove), "history-remove");
        g_object_set_data_full(G_OBJECT(remove), "id", g_strdup(entry->id), g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(delete_recent), view);
        gtk_box_pack_start(GTK_BOX(card), remove, FALSE, FALSE, 0);
        gtk_flow_box_insert(GTK_FLOW_BOX(view->flow), card, -1);
    }
    gtk_widget_show_all(view->flow);
    gtk_widget_set_visible(view->empty, entries->len == 0);
}

GtkWidget *client_history_view_new(ClientHistoryConnect connect, gpointer user_data,
                                  gboolean dx, gboolean fullscreen) {
    HistoryView *view = g_new0(HistoryView, 1);
    view->connect = connect; view->user_data = user_data;
    view->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_name(view->root, "connection-home");
    g_object_set_data_full(G_OBJECT(view->root), "history-view", view, g_free);
    gtk_box_pack_start(GTK_BOX(view->root), text_label("我的远程桌面", "history-heading"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->root), text_label("选择最近使用的桌面，或输入地址开始新的连接。", "history-muted"), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    view->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(view->entry), "spice://服务器地址:端口");
    gtk_widget_set_hexpand(view->entry, TRUE);
    g_signal_connect(view->entry, "activate", G_CALLBACK(connect_address), view);
    gtk_box_pack_start(GTK_BOX(row), view->entry, TRUE, TRUE, 0);
    GtkWidget *button = gtk_button_new_with_label("连接桌面");
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "suggested-action");
    g_signal_connect(button, "clicked", G_CALLBACK(connect_address), view);
    gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->root), row, FALSE, FALSE, 0);
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    view->dx = gtk_check_button_new_with_label("DX / 整屏 H.264");
    gtk_widget_set_tooltip_text(view->dx, "仅用于已配置整屏 H.264 的服务器");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->dx), dx);
    view->low_latency = gtk_check_button_new_with_label("低延迟（优先操作）");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->low_latency),
        g_strcmp0(g_getenv("SPICE_DX_LOW_LATENCY"), "0") != 0);
    gtk_widget_set_tooltip_text(view->low_latency,
        "立即显示解码后的画面，音频可能稍晚。关闭后按媒体时钟等待，以优先保持音画同步。");
    g_object_bind_property(view->dx, "active", view->low_latency, "sensitive", G_BINDING_SYNC_CREATE);
    view->fullscreen = gtk_check_button_new_with_label("全屏打开");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->fullscreen), fullscreen);
    gtk_box_pack_start(GTK_BOX(options), view->dx, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(options), view->low_latency, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(options), view->fullscreen, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->root), options, FALSE, FALSE, 0);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    view->count = text_label("最近连接", "history-section");
    gtk_box_pack_start(GTK_BOX(heading), view->count, TRUE, TRUE, 0);
    view->clear = gtk_button_new_with_label("清空历史");
    gtk_style_context_add_class(gtk_widget_get_style_context(view->clear), "history-remove");
    g_signal_connect(view->clear, "clicked", G_CALLBACK(clear_history), view);
    gtk_box_pack_end(GTK_BOX(heading), view->clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->root), heading, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_size_request(scroll, -1, 300);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    view->flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(view->flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(view->flow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(view->flow), 3);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(view->flow), 16);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(view->flow), 16);
    gtk_widget_set_valign(view->flow, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), view->flow, FALSE, FALSE, 0);
    view->empty = gtk_label_new("还没有连接记录\n成功连接后，桌面预览会显示在这里。");
    gtk_style_context_add_class(gtk_widget_get_style_context(view->empty), "history-muted");
    gtk_widget_set_margin_top(view->empty, 56);
    gtk_widget_set_no_show_all(view->empty, TRUE);
    gtk_box_pack_start(GTK_BOX(content), view->empty, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(scroll), content);
    gtk_box_pack_start(GTK_BOX(view->root), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(view->root), text_label("历史和桌面缩略图仅保存在本机 · 最多保留 50 台桌面", "history-muted"), FALSE, FALSE, 0);
    refresh_view(view);
    return view->root;
}
