#include "project-actions.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"

#include "core/frontmatter-c.h"
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
    ui_state_remember_session(state);
}

/* ── metadata ────────────────────────────────────────────────────────────── */

/* The file a field actually lands in: the document itself, or the sidecar
 * inside the folder. Caller frees with g_free(). */
static char* metadata_file_for(const InspectorEdit* edit)
{
    if (!edit->is_folder) {
        return g_strdup(edit->path);
    }

    char* sidecar = wordsmith_folder_metadata_path(edit->path);
    char* copy    = g_strdup(sidecar);
    wordsmith_free_string(sidecar);
    return copy;
}

/* Apply one edit to `text`. A folder's sidecar is bare YAML and a document's
 * frontmatter is fenced, which is the whole of the difference. */
static char* apply_edit(const InspectorEdit* edit, const char* text)
{
    if (edit->items != NULL) {
        return edit->is_folder
                   ? wordsmith_frontmatter_set_sequence_yaml(text, edit->key,
                                                             edit->items,
                                                             edit->item_count)
                   : wordsmith_frontmatter_set_sequence(text, edit->key, edit->items,
                                                        edit->item_count);
    }
    return edit->is_folder
               ? wordsmith_frontmatter_set_field_yaml(text, edit->key, edit->value)
               : wordsmith_frontmatter_set_field(text, edit->key, edit->value);
}

/* What `key` holds in the file behind `path` right now, so that setting it can
 * be taken back. Absent — no file, no frontmatter, no such key — comes back as
 * both fields NULL, which is a value in its own right: an author who empties a
 * field removes it, and undo has to be able to put the difference back. */
typedef struct MetadataValue {
    char*  scalar;
    char** items;   /* NULL-terminated */
} MetadataValue;

static void metadata_value_clear(MetadataValue* value)
{
    g_clear_pointer(&value->scalar, g_free);
    g_clear_pointer(&value->items, g_strfreev);
}

static void read_metadata_value(const InspectorEdit* edit, MetadataValue* out)
{
    out->scalar = NULL;
    out->items  = NULL;

    char* target = metadata_file_for(edit);
    if (target == NULL) {
        return;
    }

    char* error = NULL;
    char* text  = wordsmith_document_read(target, &error);
    wordsmith_free_string(error);
    g_free(target);
    if (text == NULL) {
        return;
    }

    /* A folder's sidecar is bare YAML; a document's frontmatter is fenced. The
     * same difference the write side turns on. */
    WordsmithFrontmatter* frontmatter =
        edit->is_folder ? wordsmith_frontmatter_parse_yaml(text)
                        : wordsmith_frontmatter_parse(text);
    if (frontmatter != NULL) {
        switch (wordsmith_frontmatter_value_kind(frontmatter, edit->key)) {
        case WORDSMITH_FRONTMATTER_SCALAR:
            out->scalar =
                g_strdup(wordsmith_frontmatter_string(frontmatter, edit->key));
            break;
        case WORDSMITH_FRONTMATTER_SEQUENCE: {
            const size_t count =
                wordsmith_frontmatter_sequence_count(frontmatter, edit->key);
            out->items = g_new0(char*, count + 1);
            for (size_t index = 0; index < count; index++) {
                out->items[index] = g_strdup(
                    wordsmith_frontmatter_sequence_at(frontmatter, edit->key, index));
            }
            break;
        }
        default:
            /* Missing, or a map the inspector has no way to have typed. */
            break;
        }
        wordsmith_frontmatter_free(frontmatter);
    }

    wordsmith_free_string(text);
}

/* The write itself, with nothing recorded. Both a typed edit and an undo of one
 * come through here, so the ordering against the editor's stale prologue is
 * written down once. */
static gboolean write_metadata(WordsmithUiState* state, const InspectorEdit* edit)
{
    /* Commit the buffer before rewriting the file under it: the editor puts the
     * frontmatter back as it found it, so saving afterwards would undo this. */
    const gboolean open_here =
        !edit->is_folder
        && g_strcmp0(edit->path, editor_panel_path(state->editor)) == 0;
    if (open_here) {
        project_actions_save(state);
    }

    char* target = metadata_file_for(edit);
    if (target == NULL) {
        return FALSE;
    }

    char* error = NULL;
    char* text  = wordsmith_document_read(target, &error);
    if (text == NULL && !edit->is_folder) {
        ui_state_report_error(state, "Could not read the document", error);
        g_free(target);
        return FALSE;
    }
    /* A folder with no sidecar yet is not a failure: writing the first field is
     * what creates it. */
    wordsmith_free_string(error);
    error = NULL;

    char* updated = apply_edit(edit, text != NULL ? text : "");
    wordsmith_free_string(text);

    if (updated == NULL) {
        ui_state_report_error(state, "Could not update the metadata", NULL);
        g_free(target);
        return FALSE;
    }

    const int ok = wordsmith_document_write(target, updated, &error);
    wordsmith_free_string(updated);
    g_free(target);

    if (!ok) {
        ui_state_report_error(state, "Could not save the metadata", error);
    } else if (open_here) {
        editor_panel_refresh_frontmatter(state->editor);
    }

    /* Either way, show what is on disk rather than what was typed. */
    inspector_panel_reload(state->inspector);
    return ok != 0;
}

void project_actions_set_metadata(WordsmithUiState* state, const InspectorEdit* edit)
{
    if (edit == NULL || edit->path == NULL || edit->key == NULL) {
        return;
    }

    /* Read before writing: afterwards the old value is only in this record.
     * Reading costs one read of a file that is about to be read again anyway. */
    MetadataValue before = { NULL, NULL };
    read_metadata_value(edit, &before);

    if (!write_metadata(state, edit)) {
        metadata_value_clear(&before);
        return;
    }

    /* InspectorEdit carries a count; a record carries a NULL-terminated vector,
     * so that a value read back out of one looks the same however it arrived. */
    char** after_items = NULL;
    if (edit->items != NULL) {
        after_items = g_new0(char*, edit->item_count + 1);
        for (size_t index = 0; index < edit->item_count; index++) {
            after_items[index] = g_strdup(edit->items[index]);
        }
    }

    /* Keyed by the item rather than the file it lands in, so a folder's fields
     * and a document's sit in the history of the row the author selected. */
    UndoRecord* record = undo_record_new_metadata(
        edit->path, edit->is_folder, edit->key, before.scalar,
        (const char* const*) before.items, edit->value,
        (const char* const*) after_items);
    if (record != NULL) {
        undo_store_push(state->undo, edit->path, record);
        ui_state_undo_changed(state);
    }

    g_strfreev(after_items);
    metadata_value_clear(&before);
}

void project_actions_apply_metadata_record(WordsmithUiState* state,
                                           const UndoRecord* record,
                                           gboolean reverse)
{
    if (state == NULL || record == NULL || record->kind != UNDO_METADATA) {
        return;
    }

    const UndoValue* value =
        reverse ? &record->metadata.before : &record->metadata.after;

    const InspectorEdit edit = {
        .path       = record->metadata.target,
        .is_folder  = record->metadata.is_folder,
        .key        = record->metadata.key,
        .value      = value->scalar,
        .items      = (const char* const*) value->items,
        .item_count = value->items != NULL ? g_strv_length(value->items) : 0,
    };
    write_metadata(state, &edit);
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

/* ── moving items ────────────────────────────────────────────────────────── */

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

/* Rebuild the binder around a completed move, and leave the moved item both
 * selected and, if the editor was showing it, still open. `was_open` is the
 * editor's path from before the move. */
static void settle_after_move(WordsmithUiState* state, const char* from,
                              const char* to, const char* was_open)
{
    ui_state_reload_project(state);

    char* reopen = remap_open_document(was_open, from, to);
    if (reopen != NULL) {
        project_actions_open_document(state, reopen);
        binder_panel_select_path(state->binder, reopen);
        g_free(reopen);
    } else {
        binder_panel_select_path(state->binder, to);
    }
}

/* Commit before any move: afterwards the editor's path no longer exists, and
 * saving into it would either fail or strand the text somewhere odd. The
 * caller frees the returned path. */
static char* save_and_remember_open(WordsmithUiState* state)
{
    project_actions_save(state);
    return g_strdup(editor_panel_path(state->editor));
}

void project_actions_move_into(WordsmithUiState* state, const char* source,
                               const char* folder)
{
    if (state->project == NULL || source == NULL || folder == NULL) {
        return;
    }

    char* was_open = save_and_remember_open(state);

    char* moved = NULL;
    char* error = NULL;
    if (!wordsmith_project_move(state->project, source, folder, &moved, &error)) {
        ui_state_report_error(state, "Could not move the item", error);
    } else {
        settle_after_move(state, source, moved, was_open);
    }

    wordsmith_free_string(moved);
    g_free(was_open);
}

void project_actions_move_beside(WordsmithUiState* state, const char* source,
                                 const char* anchor, int after)
{
    if (state->project == NULL || source == NULL || anchor == NULL) {
        return;
    }

    char* was_open = save_and_remember_open(state);

    char* moved = NULL;
    char* error = NULL;
    if (!wordsmith_project_move_beside(state->project, source, anchor, after, &moved,
                                        &error)) {
        ui_state_report_error(state, "Could not move the item", error);
    } else {
        settle_after_move(state, source, moved, was_open);
    }

    wordsmith_free_string(moved);
    g_free(was_open);
}

static void create_folder_with_selection_named(WordsmithUiState* state,
                                               const char* name, const char* item)
{
    char* was_open = save_and_remember_open(state);

    char* folder = NULL;
    char* moved = NULL;
    char* error = NULL;
    if (!wordsmith_project_group_into_new_folder(state->project, item, name, &folder,
                                                  &moved, &error)) {
        ui_state_report_error(state, "Could not gather the item into a new folder",
                              error);
    } else {
        settle_after_move(state, item, moved, was_open);
    }

    wordsmith_free_string(moved);
    wordsmith_free_string(folder);
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

/* ── renaming items ──────────────────────────────────────────────────────── */

void project_actions_rename(WordsmithUiState* state, const char* path)
{
    if (state->project == NULL) {
        return;
    }
    binder_panel_begin_rename(state->binder, path);
}

void project_actions_rename_to(WordsmithUiState* state, const char* path,
                               const char* new_name)
{
    if (state->project == NULL || path == NULL || new_name == NULL) {
        return;
    }

    char* was_open = save_and_remember_open(state);

    char* renamed = NULL;
    char* error = NULL;
    if (!wordsmith_project_rename(state->project, path, new_name, &renamed, &error)) {
        ui_state_report_error(state, "Could not rename the item", error);
    } else {
        /* The same settling a move needs, and for the same reason: the file the
         * editor is holding has just been given another name. */
        settle_after_move(state, path, renamed, was_open);
    }

    wordsmith_free_string(renamed);
    g_free(was_open);
}
