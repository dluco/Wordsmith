#include "binder-panel.h"

struct BinderPanel {
    GtkWidget* root; /* borrowed once parented into the window */
};

BinderPanel* binder_panel_new(void)
{
    BinderPanel* binder = g_new0(BinderPanel, 1);

    GtkWidget* placeholder = gtk_label_new("Binder");
    gtk_widget_add_css_class(placeholder, "pane-placeholder");
    gtk_widget_set_vexpand(placeholder, TRUE);
    gtk_widget_set_hexpand(placeholder, TRUE);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), placeholder);

    binder->root = scroller;
    return binder;
}

void binder_panel_free(BinderPanel* binder)
{
    if (binder == NULL) {
        return;
    }
    g_free(binder);
}

GtkWidget* binder_panel_widget(BinderPanel* binder)
{
    return binder != NULL ? binder->root : NULL;
}
