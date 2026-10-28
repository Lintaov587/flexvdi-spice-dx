#pragma once
#include "client-conn.h"
#include <gtk/gtk.h>

#define DISPLAY_TYPE_LAYOUT (display_layout_get_type())
G_DECLARE_FINAL_TYPE(DisplayLayout, display_layout, DISPLAY, LAYOUT, GObject)
typedef struct { gint x, y, width, height, input_id, id; gboolean auto_resize; } RemoteMonitor;
DisplayLayout *display_layout_new(ClientConn *conn);
const GArray *display_layout_get_monitors(DisplayLayout *layout);
void display_layout_show(DisplayLayout *layout, GtkWindow *parent);
gboolean display_layout_should_reveal(DisplayLayout *layout);
void display_layout_resize(DisplayLayout *layout, gint id, gint width, gint height);
