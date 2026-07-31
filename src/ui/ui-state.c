#include "ui-state.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"
#include "menu-bar.h"

#include "core/markup-c.h"

WordsmithUiState* ui_state_new(void)
{
    return g_new0(WordsmithUiState, 1);
}

void ui_state_free(WordsmithUiState* state)
{
    if (state == NULL) {
        return;
    }

    menu_bar_free(state->menu_bar);
    binder_panel_free(state->binder);
    editor_panel_free(state->editor);
    inspector_panel_free(state->inspector);

    wordsmith_project_free(state->project);

    g_free(state);
}

void ui_state_set_project(WordsmithUiState* state, WordsmithProject* project)
{
    editor_panel_close(state->editor);

    wordsmith_project_free(state->project);
    state->project = project;

    binder_panel_set_project(state->binder, project);
    ui_state_update_title(state);
}

void ui_state_reload_project(WordsmithUiState* state)
{
    if (state->project == NULL) {
        return;
    }
    wordsmith_project_reload(state->project);
    binder_panel_reload(state->binder);
}

void ui_state_update_title(WordsmithUiState* state)
{
    if (state->window == NULL) {
        return;
    }

    if (state->project == NULL) {
        gtk_window_set_title(state->window, "Wordsmith");
        return;
    }

    const char* project_title = wordsmith_project_title(state->project);
    const char* document_path = editor_panel_path(state->editor);

    if (document_path == NULL) {
        char* title = g_strdup_printf("%s - Wordsmith", project_title);
        gtk_window_set_title(state->window, title);
        g_free(title);
        return;
    }

    char* base = g_path_get_basename(document_path);
    /* Strip the .md, which is an implementation detail from the author's side. */
    char* dot = g_strrstr(base, ".");
    if (dot != NULL && dot != base) {
        *dot = '\0';
    }

    char* title = g_strdup_printf("%s%s - %s - Wordsmith",
                                  editor_panel_is_modified(state->editor) ? "*" : "",
                                  base, project_title);
    gtk_window_set_title(state->window, title);
    g_free(title);
    g_free(base);
}

void ui_state_report_error(WordsmithUiState* state, const char* message,
                           char* detail)
{
    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", message);
    if (detail != NULL) {
        gtk_alert_dialog_set_detail(dialog, detail);
    }
    gtk_alert_dialog_show(dialog, state != NULL ? state->window : NULL);
    g_object_unref(dialog);

    wordsmith_free_string(detail);
}
