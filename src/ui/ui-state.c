#include "ui-state.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "format-bar.h"
#include "inspector-panel.h"
#include "menu-bar.h"
#include "project-actions.h"
#include "spell-check.h"

#include "core/markup-c.h"
#include "core/session-c.h"

/* How long a change to the view waits before it is written. Long enough that
 * twisting open a run of folders costs one write, short enough that a crash
 * loses at most the last gesture. The balance sits where it does because what
 * is at stake is a collapsed binder rather than any of the author's words. */
#define SESSION_SAVE_DELAY_MS 1000

WordsmithUiState* ui_state_new(void)
{
    WordsmithUiState* state = g_new0(WordsmithUiState, 1);
    state->undo = undo_store_new();
    return state;
}

void ui_state_free(WordsmithUiState* state)
{
    if (state == NULL) {
        return;
    }

    /* Before the panels the pending write would read from. */
    g_clear_handle_id(&state->session_source, g_source_remove);

    menu_bar_free(state->menu_bar);
    binder_panel_free(state->binder);
    format_bar_free(state->format_bar);
    editor_panel_free(state->editor);
    inspector_panel_free(state->inspector);

    /* After the editor, which holds a borrowed pointer to it. */
    undo_store_free(state->undo);

    wordsmith_project_free(state->project);

    g_free(state);
}

/* ── panes ───────────────────────────────────────────────────────────────── */

/* Move a toggle's check mark to match what just happened to its pane.
 *
 * The menu item's handler calls into ui_state rather than moving the pane
 * itself, so a restore from the session moves the mark by the same code a click
 * does. Setting the state does not emit "change-state", so this does not come
 * back around through that handler. */
static void set_toggle_state(WordsmithUiState* state, const char* name, gboolean on)
{
    if (state->window == NULL) {
        return;
    }

    GAction* action = g_action_map_lookup_action(G_ACTION_MAP(state->window), name);
    if (action != NULL) {
        g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(on));
    }
}

/* All three answer the same two questions the same way, and the second one is
 * the subtle one: while composing, a pane is held off screen whatever the
 * answer is, so a change to it only decides what comes back at the end. Moving
 * the pane here would put it on screen in the middle of the mode — which is
 * what restoring a session does, and Ctrl+O still works with the menu bar
 * hidden. */
void ui_state_set_binder_visible(WordsmithUiState* state, gboolean visible)
{
    set_toggle_state(state, "show-binder", visible);

    if (state->composing) {
        state->binder_shown_before_composing = visible;
    } else {
        binder_panel_set_visible(state->binder, visible);
    }

    ui_state_remember_session(state);
}

void ui_state_set_inspector_visible(WordsmithUiState* state, gboolean visible)
{
    set_toggle_state(state, "show-inspector", visible);

    if (state->composing) {
        state->inspector_shown_before_composing = visible;
    } else {
        inspector_panel_set_visible(state->inspector, visible);
    }

    ui_state_remember_session(state);
}

void ui_state_set_format_bar_visible(WordsmithUiState* state, gboolean visible)
{
    set_toggle_state(state, "show-format-bar", visible);

    if (state->composing) {
        state->format_bar_shown_before_composing = visible;
    } else {
        format_bar_set_visible(state->format_bar, visible);
    }

    ui_state_remember_session(state);
}

/* The same shape as the three above — the check mark and the thing itself both
 * move from here — with two differences that follow from this being a
 * preference rather than session state. It is written to the config file
 * instead of the session, so there is nothing to remember per project; and a
 * failed write is worth a word, because the marking is right for this sitting
 * and wrong after a restart. Composition mode has no opinion: it hides panes,
 * and the manuscript keeps its spelling. */
void ui_state_set_spell_check(WordsmithUiState* state, gboolean enabled)
{
    set_toggle_state(state, "spell-check", enabled);
    editor_panel_set_spell_check(state->editor, enabled);

    char* error = NULL;
    if (!spell_check_set_wanted(enabled, &error)) {
        ui_state_report_error(state, "Cannot save the spelling preference", error);
    }
}

/* The same again, one step shorter: nothing on screen answers to this, so there
 * is no panel to tell. A failed write is worth the same word for the same
 * reason — the answer holds for this sitting and is gone after a restart. */
void ui_state_set_autoformat_lists(WordsmithUiState* state, gboolean enabled)
{
    set_toggle_state(state, "autoformat-lists", enabled);

    char* error = NULL;
    if (!editor_autoformat_set_lists(enabled, &error)) {
        ui_state_report_error(state, "Cannot save the automatic lists preference",
                              error);
    }
}

/* ── composition mode ────────────────────────────────────────────────────── */

void ui_state_set_composition_mode(WordsmithUiState* state, gboolean composing)
{
    if (state->composing == composing) {
        return;
    }
    state->composing = composing;

    if (composing) {
        /* Before hiding them, so leaving puts back what the author had rather
         * than what composition mode left. */
        state->binder_shown_before_composing =
            binder_panel_is_visible(state->binder);
        state->inspector_shown_before_composing =
            inspector_panel_is_visible(state->inspector);
        state->format_bar_shown_before_composing =
            format_bar_is_visible(state->format_bar);

        binder_panel_set_visible(state->binder, FALSE);
        inspector_panel_set_visible(state->inspector, FALSE);
        format_bar_set_visible(state->format_bar, FALSE);
    } else {
        binder_panel_set_visible(state->binder,
                                 state->binder_shown_before_composing);
        inspector_panel_set_visible(state->inspector,
                                    state->inspector_shown_before_composing);
        format_bar_set_visible(state->format_bar,
                               state->format_bar_shown_before_composing);
    }

    editor_panel_set_composition(state->editor, composing);

    if (state->window == NULL) {
        return;
    }
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(state->window),
                                            !composing);
    if (composing) {
        gtk_window_fullscreen(state->window);
    } else {
        gtk_window_unfullscreen(state->window);
    }
}

gboolean ui_state_in_composition_mode(WordsmithUiState* state)
{
    return state != NULL && state->composing;
}

/* ── session ─────────────────────────────────────────────────────────────── */

/* The author's standing answer about each pane and the format bar, which is not
 * the same as whether it is on screen: composition mode holds them all off
 * screen without changing the answers. Reading the widgets here would write
 * "dismissed" into the session for anyone who quits from composition mode, and
 * lose them the lot. */
static WordsmithSessionPanes pane_answers(WordsmithUiState* state)
{
    if (state->composing) {
        return (WordsmithSessionPanes){
            .binder_visible = state->binder_shown_before_composing,
            .inspector_visible = state->inspector_shown_before_composing,
            .format_bar_visible = state->format_bar_shown_before_composing,
        };
    }
    return (WordsmithSessionPanes){
        .binder_visible = binder_panel_is_visible(state->binder),
        .inspector_visible = inspector_panel_is_visible(state->inspector),
        .format_bar_visible = format_bar_is_visible(state->format_bar),
    };
}

static void save_session_now(WordsmithUiState* state)
{
    if (state->project == NULL) {
        return;
    }

    char** expanded = binder_panel_expanded_paths(state->binder);

    char* error = NULL;
    if (!wordsmith_session_save(wordsmith_project_root(state->project),
                                editor_panel_path(state->editor),
                                (const char* const*) expanded,
                                g_strv_length(expanded),
                                pane_answers(state),
                                &error)) {
        /* A view that does not come back is not worth a dialog in the author's
         * way, and there is nothing they could do about it if it were. */
        g_warning("could not remember the view: %s",
                  error != NULL ? error : "unknown error");
    }

    wordsmith_free_string(error);
    g_strfreev(expanded);
}

static gboolean on_session_save_timeout(gpointer user_data)
{
    WordsmithUiState* state = user_data;
    state->session_source = 0;
    save_session_now(state);
    return G_SOURCE_REMOVE;
}

void ui_state_remember_session(WordsmithUiState* state)
{
    if (state->project == NULL || state->restoring_session) {
        return;
    }
    /* The first change starts the clock and the rest ride along with it, so a
     * burst settles one second after it began rather than one second after it
     * stops. */
    if (state->session_source == 0) {
        state->session_source =
            g_timeout_add(SESSION_SAVE_DELAY_MS, on_session_save_timeout, state);
    }
}

void ui_state_flush_session(WordsmithUiState* state)
{
    g_clear_handle_id(&state->session_source, g_source_remove);
    if (!state->restoring_session) {
        save_session_now(state);
    }
}

/* Put back what was saved for the project now open. Anything recorded that has
 * since left the disk was already dropped by the core, so a folder deleted
 * outside Wordsmith costs nothing here. */
static void restore_session(WordsmithUiState* state)
{
    if (state->project == NULL) {
        return;
    }

    WordsmithSession* session =
        wordsmith_session_load(wordsmith_project_root(state->project));

    const size_t count = wordsmith_session_expanded_count(session);
    char** expanded = g_new0(char*, count + 1);
    for (size_t index = 0; index < count; index++) {
        expanded[index] = g_strdup(wordsmith_session_expanded(session, index));
    }

    state->restoring_session = TRUE;
    binder_panel_set_expanded_paths(state->binder, (const char* const*) expanded);
    /* Selecting the row is what opens the document, exactly as clicking it
     * would, so the editor and the inspector follow without being told
     * separately. A NULL path selects nothing. */
    binder_panel_select_path(state->binder, wordsmith_session_open_document(session));
    /* After the selection, which loads the inspector but does not decide
     * whether either pane is on screen. The guard keeps this from counting as a
     * change to write straight back out. */
    const WordsmithSessionPanes panes = wordsmith_session_panes(session);
    ui_state_set_binder_visible(state, panes.binder_visible);
    ui_state_set_inspector_visible(state, panes.inspector_visible);
    ui_state_set_format_bar_visible(state, panes.format_bar_visible);
    state->restoring_session = FALSE;

    g_strfreev(expanded);
    wordsmith_session_free(session);
}

void ui_state_set_project(WordsmithUiState* state, WordsmithProject* project)
{
    /* While the outgoing project's root is still there to key it by. */
    ui_state_flush_session(state);

    editor_panel_close(state->editor);
    inspector_panel_clear(state->inspector);

    /* After the editor has been put away, so nothing is left holding a history
     * that has just gone. Undo ends with the project: it is not written down,
     * and a history is only meaningful against the files it was made over. */
    undo_store_clear(state->undo);

    wordsmith_project_free(state->project);
    state->project = project;

    binder_panel_set_project(state->binder, project);
    restore_session(state);
    ui_state_update_title(state);
    ui_state_undo_changed(state);
}

void ui_state_reload_project(WordsmithUiState* state)
{
    if (state->project == NULL) {
        return;
    }
    wordsmith_project_reload(state->project);
    binder_panel_reload(state->binder);
}

/* ── undo ────────────────────────────────────────────────────────────────── */

/* Which item's history a press addresses: the one the author is looking at.
 *
 * The binder's selection rather than the open document, because a folder has a
 * history too — it has no body to edit, but it has metadata — and because the
 * selection is what both side panes already follow. The open document is the
 * fallback for the one case where there is no selection to read, a document
 * opened from the command line before any row has been clicked. Caller frees. */
static char* undo_target(WordsmithUiState* state)
{
    if (state == NULL || state->project == NULL) {
        return NULL;
    }

    char* selected = binder_panel_selected_path(state->binder);
    if (selected != NULL) {
        return selected;
    }
    return g_strdup(editor_panel_path(state->editor));
}

static void step(WordsmithUiState* state, gboolean reverse)
{
    char* path = undo_target(state);
    if (path == NULL) {
        return;
    }

    const UndoRecord* record = reverse ? undo_store_peek_undo(state->undo, path)
                                       : undo_store_peek_redo(state->undo, path);
    if (record == NULL) {
        g_free(path);
        return;
    }

    if (record->kind == UNDO_METADATA) {
        project_actions_apply_metadata_record(state, record, reverse);
    } else {
        editor_panel_apply_record(state->editor, record, reverse);
    }

    /* After applying, so a record that could not be put back leaves the history
     * where it was rather than stepping over it. */
    if (reverse) {
        undo_store_step_undo(state->undo, path);
    } else {
        undo_store_step_redo(state->undo, path);
    }

    ui_state_undo_changed(state);
    ui_state_update_title(state);
    g_free(path);
}

void ui_state_undo(WordsmithUiState* state)
{
    step(state, TRUE);
}

void ui_state_redo(WordsmithUiState* state)
{
    step(state, FALSE);
}

void ui_state_undo_changed(WordsmithUiState* state)
{
    if (state == NULL) {
        return;
    }

    char* path = undo_target(state);
    char* undo_verb = undo_record_verb(undo_store_peek_undo(state->undo, path));
    char* redo_verb = undo_record_verb(undo_store_peek_redo(state->undo, path));

    menu_bar_show_undo(state->menu_bar, undo_verb, redo_verb);

    g_free(undo_verb);
    g_free(redo_verb);
    g_free(path);
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
