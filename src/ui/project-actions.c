#include "project-actions.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"

#include "core/markup-c.h"
#include "core/project-c.h"

#include <string.h>

/* ── name prompt ─────────────────────────────────────────────────────────── */

/* What to do with the name the user typed. `target` is the folder to create in,
 * or the item to gather up, decided when the prompt opened. */
typedef void (*NameEnteredFn)(WordsmithUiState* state, const char* name,
                              const char* target);

typedef struct NamePrompt {
    WordsmithUiState* state;
    GtkWindow*         window;
    GtkEntry*          entry;
    NameEnteredFn      on_entered;
    char*              target;   /* owned */
} NamePrompt;

static void name_prompt_accept(NamePrompt* prompt)
{
    const char* text = gtk_editable_get_text(GTK_EDITABLE(prompt->entry));
    if (text != NULL && text[0] != '\0') {
        prompt->on_entered(prompt->state, text, prompt->target);
    }
    gtk_window_destroy(prompt->window);
}

static void on_name_prompt_activate(GtkEntry* entry, gpointer user_data)
{
    (void) entry;
    name_prompt_accept(user_data);
}

static void on_name_prompt_create(GtkButton* button, gpointer user_data)
{
    (void) button;
    name_prompt_accept(user_data);
}

static void on_name_prompt_cancel(GtkButton* button, gpointer user_data)
{
    (void) button;
    NamePrompt* prompt = user_data;
    gtk_window_destroy(prompt->window);
}

static void on_name_prompt_destroy(GtkWidget* widget, gpointer user_data)
{
    (void) widget;
    NamePrompt* prompt = user_data;
    g_free(prompt->target);
    g_free(prompt);
}

/* A small modal asking for one name. GtkAlertDialog has no text entry, so this
 * is a plain window rather than anything fancier. `target` is copied. */
static void show_name_prompt(WordsmithUiState* state, const char* title,
                             const char* placeholder, const char* target,
                             NameEnteredFn on_entered)
{
    if (target == NULL) {
        return;
    }

    NamePrompt* prompt = g_new0(NamePrompt, 1);
    prompt->state      = state;
    prompt->on_entered = on_entered;
    prompt->target     = g_strdup(target);

    GtkWidget* window = gtk_window_new();
    prompt->window = GTK_WINDOW(window);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_transient_for(GTK_WINDOW(window), state->window);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), 360, -1);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget* entry = gtk_entry_new();
    prompt->entry = GTK_ENTRY(entry);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    GtkWidget* create = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), create);
    gtk_box_append(GTK_BOX(box), buttons);

    gtk_window_set_child(GTK_WINDOW(window), box);

    g_signal_connect(entry, "activate", G_CALLBACK(on_name_prompt_activate), prompt);
    g_signal_connect(create, "clicked", G_CALLBACK(on_name_prompt_create), prompt);
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_name_prompt_cancel), prompt);
    g_signal_connect(window, "destroy", G_CALLBACK(on_name_prompt_destroy), prompt);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(entry);
}

/* ── opening and creating projects ───────────────────────────────────────── */

void project_actions_open_path(WordsmithUiState* state, const char* root)
{
    char* error = NULL;
    WordsmithProject* project = wordsmith_project_open(root, &error);
    if (project == NULL) {
        ui_state_report_error(state, "Could not open project", error);
        return;
    }
    ui_state_set_project(state, project);
}

static void on_open_folder_chosen(GObject* source, GAsyncResult* result,
                                  gpointer user_data)
{
    WordsmithUiState* state = user_data;
    GError* error = NULL;

    GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
                                                         result, &error);
    if (folder == NULL) {
        /* Dismissal is not a failure worth reporting. */
        g_clear_error(&error);
        return;
    }

    char* path = g_file_get_path(folder);
    if (path != NULL) {
        project_actions_open_path(state, path);
    }
    g_free(path);
    g_object_unref(folder);
}

void project_actions_open_dialog(WordsmithUiState* state)
{
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Project");
    gtk_file_dialog_select_folder(dialog, state->window, NULL,
                                  on_open_folder_chosen, state);
    g_object_unref(dialog);
}

static void on_new_folder_chosen(GObject* source, GAsyncResult* result,
                                 gpointer user_data)
{
    WordsmithUiState* state = user_data;
    GError* error = NULL;

    GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
                                                         result, &error);
    if (folder == NULL) {
        g_clear_error(&error);
        return;
    }

    char* path = g_file_get_path(folder);
    if (path != NULL) {
        char* basename = g_path_get_basename(path);
        char* message = NULL;
        WordsmithProject* project = wordsmith_project_create(path, basename,
                                                               &message);
        if (project == NULL) {
            ui_state_report_error(state, "Could not create project", message);
        } else {
            ui_state_set_project(state, project);
        }
        g_free(basename);
    }
    g_free(path);
    g_object_unref(folder);
}

void project_actions_new_dialog(WordsmithUiState* state)
{
    /* The chooser creates or picks the directory; the project file and the
     * manuscript folder go inside whatever comes back. */
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "New Project Folder");
    gtk_file_dialog_select_folder(dialog, state->window, NULL,
                                  on_new_folder_chosen, state);
    g_object_unref(dialog);
}

void project_actions_close(WordsmithUiState* state)
{
    project_actions_save(state);
    ui_state_set_project(state, NULL);
}

/* ── documents ───────────────────────────────────────────────────────────── */

void project_actions_save(WordsmithUiState* state)
{
    if (!editor_panel_is_modified(state->editor)) {
        return;
    }

    char* error = NULL;
    if (!editor_panel_save(state->editor, &error)) {
        ui_state_report_error(state, "Could not save document", error);
        return;
    }
    ui_state_update_title(state);
}

void project_actions_open_document(WordsmithUiState* state, const char* path)
{
    if (path == NULL || g_strcmp0(path, editor_panel_path(state->editor)) == 0) {
        return;
    }

    /* Switching documents commits the outgoing one, so a click in the binder
     * never silently discards edits. */
    project_actions_save(state);

    char* error = NULL;
    if (!editor_panel_load(state->editor, path, &error)) {
        ui_state_report_error(state, "Could not open document", error);
        return;
    }
    inspector_panel_set_document(state->inspector, path);
    ui_state_update_title(state);
}

/* ── creating binder items ───────────────────────────────────────────────── */

static void create_folder_named(WordsmithUiState* state, const char* name,
                                const char* parent)
{
    char* created = NULL;
    char* error = NULL;
    if (!wordsmith_project_create_folder(state->project, parent, name, &created,
                                          &error)) {
        ui_state_report_error(state, "Could not create folder", error);
        return;
    }

    ui_state_reload_project(state);
    if (created != NULL) {
        binder_panel_select_path(state->binder, created);
    }
    wordsmith_free_string(created);
}

void project_actions_new_folder_in(WordsmithUiState* state, const char* parent)
{
    if (state->project == NULL) {
        return;
    }
    show_name_prompt(state, "New Folder", "Folder name", parent, create_folder_named);
}

void project_actions_new_folder(WordsmithUiState* state)
{
    if (state->project == NULL) {
        return;
    }
    char* parent = binder_panel_target_folder(state->binder);
    project_actions_new_folder_in(state, parent);
    g_free(parent);
}

static void create_text_named(WordsmithUiState* state, const char* name,
                              const char* parent)
{
    char* created = NULL;
    char* error = NULL;
    if (!wordsmith_project_create_document(state->project, parent, name, &created,
                                            &error)) {
        ui_state_report_error(state, "Could not create document", error);
        return;
    }

    ui_state_reload_project(state);
    if (created != NULL) {
        binder_panel_select_path(state->binder, created);
        project_actions_open_document(state, created);
    }
    wordsmith_free_string(created);
}

void project_actions_new_text_in(WordsmithUiState* state, const char* parent)
{
    if (state->project == NULL) {
        return;
    }
    show_name_prompt(state, "New Text", "Document title", parent, create_text_named);
}

void project_actions_new_text(WordsmithUiState* state)
{
    if (state->project == NULL) {
        return;
    }
    char* parent = binder_panel_target_folder(state->binder);
    project_actions_new_text_in(state, parent);
    g_free(parent);
}

/* ── gathering an item into a new folder ─────────────────────────────────── */

/* Where `open` ends up once `from` has been moved to `to`, or NULL if the move
 * does not touch it. Covers both the document itself moving and a folder
 * moving out from over it. */
static char* remap_open_document(const char* open, const char* from, const char* to)
{
    if (open == NULL) {
        return NULL;
    }
    if (g_strcmp0(open, from) == 0) {
        return g_strdup(to);
    }

    char* prefix = g_strconcat(from, G_DIR_SEPARATOR_S, NULL);
    char* moved = NULL;
    if (g_str_has_prefix(open, prefix)) {
        moved = g_build_filename(to, open + strlen(prefix), NULL);
    }
    g_free(prefix);
    return moved;
}

static void create_folder_with_selection_named(WordsmithUiState* state,
                                               const char* name, const char* item)
{
    /* Commit first: after the move the editor's path no longer exists, and
     * saving into it would either fail or strand the text somewhere odd. */
    project_actions_save(state);
    char* was_open = g_strdup(editor_panel_path(state->editor));

    char* parent = g_path_get_dirname(item);
    char* folder = NULL;
    char* error = NULL;
    if (!wordsmith_project_create_folder(state->project, parent, name, &folder,
                                          &error)) {
        ui_state_report_error(state, "Could not create folder", error);
        g_free(parent);
        g_free(was_open);
        return;
    }

    char* moved = NULL;
    if (!wordsmith_project_move(state->project, item, folder, &moved, &error)) {
        /* The folder stays: there is no delete verb yet to take it back, and
         * an empty folder is a smaller surprise than a half-done move. */
        ui_state_report_error(state, "Could not move the item into the new folder",
                              error);
    } else {
        ui_state_reload_project(state);

        char* reopen = remap_open_document(was_open, item, moved);
        if (reopen != NULL) {
            project_actions_open_document(state, reopen);
            binder_panel_select_path(state->binder, reopen);
            g_free(reopen);
        } else {
            binder_panel_select_path(state->binder, moved);
        }
    }

    wordsmith_free_string(moved);
    wordsmith_free_string(folder);
    g_free(parent);
    g_free(was_open);
}

void project_actions_new_folder_with_selection(WordsmithUiState* state,
                                               const char* item)
{
    if (state->project == NULL) {
        return;
    }
    show_name_prompt(state, "New Folder with Selection", "Folder name", item,
                     create_folder_with_selection_named);
}
