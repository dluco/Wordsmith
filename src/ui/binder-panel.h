#pragma once

#include <gtk/gtk.h>

/* The binder: the left-hand tree of the project's documents, folders and
 * research. Currently a placeholder shell. */
typedef struct BinderPanel BinderPanel;

BinderPanel* binder_panel_new(void);
void         binder_panel_free(BinderPanel* binder);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* binder_panel_widget(BinderPanel* binder);
