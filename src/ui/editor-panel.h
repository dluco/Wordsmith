#pragma once

#include <gtk/gtk.h>

/* The centre pane: the manuscript editor. Will eventually host the document
 * view, corkboard and outliner modes. Currently a placeholder shell. */
typedef struct EditorPanel EditorPanel;

EditorPanel* editor_panel_new(void);
void         editor_panel_free(EditorPanel* editor);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* editor_panel_widget(EditorPanel* editor);
