#include "editor-panel.h"

struct EditorPanel {
    GtkWidget* root; /* borrowed once parented into the window */
};

EditorPanel* editor_panel_new(void)
{
    EditorPanel* editor = g_new0(EditorPanel, 1);

    GtkWidget* placeholder = gtk_label_new("No document selected");
    gtk_widget_add_css_class(placeholder, "pane-placeholder");
    gtk_widget_set_vexpand(placeholder, TRUE);
    gtk_widget_set_hexpand(placeholder, TRUE);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "editor-pane");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), placeholder);

    editor->root = scroller;
    return editor;
}

void editor_panel_free(EditorPanel* editor)
{
    if (editor == NULL) {
        return;
    }
    g_free(editor);
}

GtkWidget* editor_panel_widget(EditorPanel* editor)
{
    return editor != NULL ? editor->root : NULL;
}
