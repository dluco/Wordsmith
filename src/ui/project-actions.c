#include "project-actions.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"

#include "core/frontmatter-c.h"
#include "core/markup-c.h"
#include "core/project-c.h"

#include <string.h>

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

/* Every one of these creates the item under a name nothing is using and then
 * opens an entry over it in the binder, rather than asking for a name in a
 * dialog first.
 *
 * The binder is a directory scan, so a row with no file behind it is a row the
 * scan cannot produce — which is why something exists on disk before the author
 * has said what it is called, rather than a placeholder standing in the tree
 * until they do. An author who dismisses the entry keeps an item called
 * Untitled, which is what every file manager that names this way leaves behind. */

void project_actions_new_folder_in(WordsmithUiState* state, const char* parent)
{
    if (state->project == NULL || parent == NULL) {
        return;
    }

    char* created = NULL;
    char* error = NULL;
    if (!wordsmith_project_create_untitled_folder(state->project, parent, &created,
                                                   &error)) {
        ui_state_report_error(state, "Could not create folder", error);
        return;
    }

    ui_state_reload_project(state);
    if (created != NULL) {
        binder_panel_select_path(state->binder, created);
        binder_panel_begin_rename(state->binder, created);
    }
    wordsmith_free_string(created);
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

void project_actions_new_text_in(WordsmithUiState* state, const char* parent)
{
    if (state->project == NULL || parent == NULL) {
        return;
    }

    char* created = NULL;
    char* error = NULL;
    if (!wordsmith_project_create_untitled_document(state->project, parent, &created,
                                                     &error)) {
        ui_state_report_error(state, "Could not create document", error);
        return;
    }

    ui_state_reload_project(state);
    if (created != NULL) {
        binder_panel_select_path(state->binder, created);
        /* Opened before the entry takes the focus, so the manuscript pane is
         * already showing the new document while its name is being typed. */
        project_actions_open_document(state, created);
        binder_panel_begin_rename(state->binder, created);
    }
    wordsmith_free_string(created);
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

void project_actions_new_folder_with_selection(WordsmithUiState* state,
                                               const char* item)
{
    if (state->project == NULL || item == NULL) {
        return;
    }

    char* was_open = save_and_remember_open(state);

    char* folder = NULL;
    char* moved = NULL;
    char* error = NULL;
    if (!wordsmith_project_group_into_untitled_folder(state->project, item, &folder,
                                                       &moved, &error)) {
        ui_state_report_error(state, "Could not gather the item into a new folder",
                              error);
    } else {
        settle_after_move(state, item, moved, was_open);
        /* The folder is the new thing here, so it is the folder that gets the
         * entry — even though it is the item inside it that settling left
         * selected and open. */
        binder_panel_select_path(state->binder, folder);
        binder_panel_begin_rename(state->binder, folder);
    }

    wordsmith_free_string(moved);
    wordsmith_free_string(folder);
    g_free(was_open);
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

/* ── deleting items ──────────────────────────────────────────────────────── */

/* Whether `path` is `item` itself or something inside it. Trashing a folder
 * takes everything under it, so the document the editor is holding may be going
 * without being named. */
static gboolean path_is_within(const char* path, const char* item)
{
    if (path == NULL || item == NULL) {
        return FALSE;
    }
    if (g_strcmp0(path, item) == 0) {
        return TRUE;
    }

    char* prefix = g_strconcat(item, G_DIR_SEPARATOR_S, NULL);
    const gboolean within = g_str_has_prefix(path, prefix);
    g_free(prefix);
    return within;
}

static void trash_confirmed(WordsmithUiState* state, const char* path)
{
    /* Committed first for the reason every move commits first, and then thrown
     * away with the file if it was this one: the words the author typed go into
     * the trashed copy rather than being lost on the way there. */
    char* was_open = save_and_remember_open(state);
    char* was_selected = binder_panel_selected_path(state->binder);

    char* trashed = NULL;
    char* error = NULL;
    if (!wordsmith_project_trash(state->project, path, &trashed, &error)) {
        ui_state_report_error(state, "Could not move the item to the trash", error);
        g_free(was_open);
        g_free(was_selected);
        return;
    }

    if (path_is_within(was_open, path)) {
        editor_panel_close(state->editor);
    }
    /* The pane follows the selection, and what it was following has gone. */
    if (path_is_within(was_selected, path) || path_is_within(was_open, path)) {
        inspector_panel_clear(state->inspector);
    }

    /* The history of something that is not in the binder any more is not
     * reachable and not worth keeping. Undo cannot bring the file back — that is
     * what the trash is for — so there is nothing here to protect. */
    undo_store_forget(state->undo, path);

    ui_state_reload_project(state);
    ui_state_update_title(state);
    ui_state_remember_session(state);
    ui_state_undo_changed(state);

    wordsmith_free_string(trashed);
    g_free(was_open);
    g_free(was_selected);
}

/* The question and its answer, since the dialog outlives the call that raised
 * it. `path` is copied for the same reason the name prompt copies its target. */
typedef struct TrashPrompt {
    WordsmithUiState* state;
    char*             path;   /* owned */
} TrashPrompt;

static void on_trash_answered(GObject* source, GAsyncResult* result,
                              gpointer user_data)
{
    TrashPrompt* prompt = user_data;
    GError* error = NULL;

    const int chosen = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source),
                                                      result, &error);
    /* Dismissing the dialog — Escape, or the window manager — is a no, and
     * arrives here as an error rather than as a button. */
    if (error == NULL && chosen == 1) {
        trash_confirmed(prompt->state, prompt->path);
    }
    g_clear_error(&error);

    g_free(prompt->path);
    g_free(prompt);
}

void project_actions_trash(WordsmithUiState* state, const char* path)
{
    if (state->project == NULL || path == NULL) {
        return;
    }

    char* name = g_path_get_basename(path);
    const gboolean is_folder = g_file_test(path, G_FILE_TEST_IS_DIR);

    char* question = g_strdup_printf("Move “%s” to the trash?", name);

    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", question);
    gtk_alert_dialog_set_detail(
        dialog,
        is_folder
            ? "The folder and everything in it are kept in the project's trash "
              "folder until you empty it by hand."
            : "It is kept in the project's trash folder until you empty it by "
              "hand.");

    const char* buttons[] = { "Cancel", "Move to Trash", NULL };
    gtk_alert_dialog_set_buttons(dialog, buttons);
    /* Cancel is both the default and what a dismissal means, so the answer that
     * costs nothing is the one a reflex gives. */
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);

    TrashPrompt* prompt = g_new0(TrashPrompt, 1);
    prompt->state = state;
    prompt->path  = g_strdup(path);

    gtk_alert_dialog_choose(dialog, state->window, NULL, on_trash_answered, prompt);

    g_object_unref(dialog);
    g_free(question);
    g_free(name);
}
