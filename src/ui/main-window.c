#include "main-window.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"
#include "menu-bar.h"
#include "ui-state.h"

/* Starting geometry. The pane positions are absolute widths from the left,
 * so the inspector divider accounts for the binder sitting to its left. */
#define WINDOW_DEFAULT_WIDTH   1280
#define WINDOW_DEFAULT_HEIGHT  800
#define BINDER_DEFAULT_WIDTH   240
#define INSPECTOR_DEFAULT_WIDTH 300

static void on_window_destroy(GtkWidget* widget, gpointer user_data)
{
    (void) widget;

    ui_state_free(user_data);
}

void main_window_present(GtkApplication* app)
{
    WordsworthUiState* state = ui_state_new();

    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Wordsworth");
    gtk_window_set_default_size(GTK_WINDOW(window),
                                WINDOW_DEFAULT_WIDTH, WINDOW_DEFAULT_HEIGHT);
    state->window = GTK_WINDOW(window);

    /* The menu bar installs its actions on the window, so it needs the window
     * in place first. */
    state->menu_bar = menu_bar_new(state, app);
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(window), TRUE);

    state->binder    = binder_panel_new();
    state->editor    = editor_panel_new();
    state->inspector = inspector_panel_new();

    /* Editor and inspector share the space to the right of the binder. */
    GtkWidget* editor_inspector_paned =
        gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(editor_inspector_paned),
                              editor_panel_widget(state->editor));
    gtk_paned_set_end_child(GTK_PANED(editor_inspector_paned),
                            inspector_panel_widget(state->inspector));
    gtk_paned_set_resize_start_child(GTK_PANED(editor_inspector_paned), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(editor_inspector_paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(editor_inspector_paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(editor_inspector_paned), FALSE);
    gtk_paned_set_position(
        GTK_PANED(editor_inspector_paned),
        WINDOW_DEFAULT_WIDTH - BINDER_DEFAULT_WIDTH - INSPECTOR_DEFAULT_WIDTH);

    GtkWidget* binder_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(binder_paned),
                              binder_panel_widget(state->binder));
    gtk_paned_set_end_child(GTK_PANED(binder_paned), editor_inspector_paned);
    gtk_paned_set_resize_start_child(GTK_PANED(binder_paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(binder_paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(binder_paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(binder_paned), FALSE);
    gtk_paned_set_position(GTK_PANED(binder_paned), BINDER_DEFAULT_WIDTH);

    gtk_window_set_child(GTK_WINDOW(window), binder_paned);

    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), state);

    gtk_window_present(GTK_WINDOW(window));
}
