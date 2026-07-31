#pragma once

#include <gtk/gtk.h>

/* The inspector: the right-hand pane of per-document metadata, synopsis,
 * notes, keywords and snapshots. Currently a placeholder shell. */
typedef struct InspectorPanel InspectorPanel;

InspectorPanel* inspector_panel_new(void);
void            inspector_panel_free(InspectorPanel* inspector);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* inspector_panel_widget(InspectorPanel* inspector);
