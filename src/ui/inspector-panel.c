#include "inspector-panel.h"

struct InspectorPanel {
    GtkWidget* root; /* borrowed once parented into the window */
};

InspectorPanel* inspector_panel_new(void)
{
    InspectorPanel* inspector = g_new0(InspectorPanel, 1);

    GtkWidget* placeholder = gtk_label_new("Inspector");
    gtk_widget_add_css_class(placeholder, "pane-placeholder");
    gtk_widget_set_vexpand(placeholder, TRUE);
    gtk_widget_set_hexpand(placeholder, TRUE);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), placeholder);

    inspector->root = scroller;
    return inspector;
}

void inspector_panel_free(InspectorPanel* inspector)
{
    if (inspector == NULL) {
        return;
    }
    g_free(inspector);
}

GtkWidget* inspector_panel_widget(InspectorPanel* inspector)
{
    return inspector != NULL ? inspector->root : NULL;
}
