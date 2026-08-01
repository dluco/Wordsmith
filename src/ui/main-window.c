#include "main-window.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"
#include "menu-bar.h"
#include "project-actions.h"
#include "ui-state.h"

/* Starting geometry. The pane positions are absolute widths from the left,
 * so the inspector divider accounts for the binder sitting to its left. */
#define WINDOW_DEFAULT_WIDTH   1280
#define WINDOW_DEFAULT_HEIGHT  800
#define BINDER_DEFAULT_WIDTH   240
#define INSPECTOR_DEFAULT_WIDTH 300

/* Clicking a document in the binder opens it. Clicking a folder moves where
 * new items will be created, and shows the folder's own fields: it has no body
 * to open, but it does have a synopsis of its own. */
static void on_binder_selected(const char* path, int is_folder, void* user_data)
{
    WordsmithUiState* state = user_data;
    if (path == NULL) {
        return;
    }
    if (is_folder) {
        inspector_panel_set_folder(state->inspector, path);
        return;
    }
    project_actions_open_document(state, path);
}

/* A drag in the binder either puts an item inside a folder or sets it down next
 * to a sibling. The panel works out which from where the pointer let go; here
 * that just picks the verb. */
static void on_binder_moved(const char* source_path, const char* target_path,
                            BinderDropZone zone, void* user_data)
{
    WordsmithUiState* state = user_data;
    if (zone == BINDER_DROP_INTO) {
        project_actions_move_into(state, source_path, target_path);
        return;
    }
    project_actions_move_beside(state, source_path, target_path,
                                zone == BINDER_DROP_AFTER);
}

/* The inspector types a field; where those bytes go, and what else has to know
 * about them, is the project's problem rather than the pane's. */
static void on_inspector_commit(const InspectorEdit* edit, void* user_data)
{
    project_actions_set_metadata(user_data, edit);
}

static void on_editor_modified(int modified, void* user_data)
{
    (void) modified;
    ui_state_update_title(user_data);
}

/* Committing on close keeps the binder-click behaviour consistent: edits are
 * never dropped without the author asking for it. */
static gboolean on_window_close_request(GtkWindow* window, gpointer user_data)
{
    (void) window;
    project_actions_save(user_data);
    return GDK_EVENT_PROPAGATE;   /* let the close proceed */
}

static void on_window_destroy(GtkWidget* widget, gpointer user_data)
{
    (void) widget;
    ui_state_free(user_data);
}

void main_window_present(GtkApplication* app, const char* initial_project)
{
    WordsmithUiState* state = ui_state_new();
    state->app = app;

    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window),
                                WINDOW_DEFAULT_WIDTH, WINDOW_DEFAULT_HEIGHT);
    state->window = GTK_WINDOW(window);
    ui_state_update_title(state);

    /* The menu bar installs its actions on the window, so it needs the window
     * in place first. */
    state->menu_bar = menu_bar_new(state, app);
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(window), TRUE);

    state->binder    = binder_panel_new();
    state->editor    = editor_panel_new();
    state->inspector = inspector_panel_new();

    binder_panel_set_select_callback(state->binder, on_binder_selected, state);
    binder_panel_set_move_callback(state->binder, on_binder_moved, state);
    editor_panel_set_modified_callback(state->editor, on_editor_modified, state);
    inspector_panel_set_commit_callback(state->inspector, on_inspector_commit, state);

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

    g_signal_connect(window, "close-request",
                     G_CALLBACK(on_window_close_request), state);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), state);

    gtk_window_present(GTK_WINDOW(window));

    /* Opened after the window is up so any failure has somewhere to report to. */
    if (initial_project != NULL) {
        project_actions_open_path(state, initial_project);
    }
}
