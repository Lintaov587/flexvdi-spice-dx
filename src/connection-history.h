/* Local connection history. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>

typedef void (*ClientHistoryConnect)(const gchar *uri, gboolean dx,
                                     gboolean low_latency, gboolean fullscreen, gpointer user_data);
gchar *client_history_normalize_uri(const gchar *input, GError **error);
GtkWidget *client_history_view_new(ClientHistoryConnect connect, gpointer user_data,
                                  gboolean dx, gboolean fullscreen);
void client_history_record(const gchar *uri, const gchar *name, gboolean dx,
                           gint64 connected_at, GdkPixbuf *desktop, gboolean first);
