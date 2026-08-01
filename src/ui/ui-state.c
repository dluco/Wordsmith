#include "ui-state.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"
#include "menu-bar.h"

#include "core/markup-c.h"
#include "core/session-c.h"

/* How long a change to the view waits before it is written. Long enough that
 * twisting open a run of folders costs one write, short enough that a crash
 * loses at most the last gesture. The balance sits where it does because what
 * is at stake is a collapsed binder rather than any of the author's words. */
#define SESSION_SAVE_DELAY_MS 1000

WordsmithUiState* ui_state_new(void)
{
    return g_new0(WordsmithUiState, 1);
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
    editor_panel_free(state->editor);
    inspector_panel_free(state->inspector);

    wordsmith_project_free(state->project);

    g_free(state);
}

/* ── panes ───────────────────────────────────────────────────────────────── */

void ui_state_set_inspector_visible(WordsmithUiState* state, gboolean visible)
{
    /* The menu item's own handler calls in here rather than doing this itself,
     * so a restore from the session moves the check mark by the same code that
     * a click does. Setting the state does not emit "change-state", so this
     * does not come back around. */
    GAction* action = state->window != NULL
        ? g_action_map_lookup_action(G_ACTION_MAP(state->window), "show-inspector")
        : NULL;
    if (action != NULL) {
        g_simple_action_set_state(G_SIMPLE_ACTION(action),
                                  g_variant_new_boolean(visible));
    }

    /* While composing, both panes are held off screen whatever the answer is,
     * so a change to it only decides what comes back at the end. Moving the
     * pane here would put the inspector on screen in the middle of the mode —
     * which is what restoring a session does, and Ctrl+O still works with the
     * menu bar hidden. */
    if (state->composing) {
        state->inspector_shown_before_composing = visible;
    } else {
        inspector_panel_set_visible(state->inspector, visible);
    }

    ui_state_remember_session(state);
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

        binder_panel_set_visible(state->binder, FALSE);
        inspector_panel_set_visible(state->inspector, FALSE);
    } else {
        binder_panel_set_visible(state->binder,
                                 state->binder_shown_before_composing);
        inspector_panel_set_visible(state->inspector,
                                    state->inspector_shown_before_composing);
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

/* The author's standing answer about the inspector, which is not the same as
 * whether the pane is on screen: composition mode holds it off screen without
 * changing the answer. Reading the widget here would write "dismissed" into the
 * session for anyone who quits from composition mode, and lose them the pane. */
static gboolean inspector_answer(WordsmithUiState* state)
{
    return state->composing ? state->inspector_shown_before_composing
                            : inspector_panel_is_visible(state->inspector);
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
                                inspector_answer(state),
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
    /* After the selection, which loads the pane but does not decide whether it
     * is on screen. The guard keeps this from counting as a change to write
     * straight back out. */
    ui_state_set_inspector_visible(
        state, wordsmith_session_inspector_visible(session));
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

    wordsmith_project_free(state->project);
    state->project = project;

    binder_panel_set_project(state->binder, project);
    restore_session(state);
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
