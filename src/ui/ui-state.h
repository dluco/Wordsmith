#pragma once

#include "core/project-c.h"
#include "undo-stack.h"

#include <gtk/gtk.h>

typedef struct BinderPanel    BinderPanel;
typedef struct EditorPanel    EditorPanel;
typedef struct FormatBar      FormatBar;
typedef struct InspectorPanel InspectorPanel;
typedef struct MenuBar        MenuBar;

/* Shared per-window state. Panels borrow this and never own it; it is freed
 * when the window it belongs to is destroyed. */
typedef struct WordsmithUiState {
    GtkWindow*      window;
    GtkApplication* app;      /* borrowed */

    WordsmithProject* project;   /* owned; NULL when no project is open */

    MenuBar*        menu_bar;
    BinderPanel*    binder;
    FormatBar*      format_bar;
    EditorPanel*    editor;
    InspectorPanel* inspector;

    /* Every item's undo history, for as long as the project is open. Owned;
     * cleared when the project changes, which is the whole of what makes undo
     * session-scoped. See undo-stack.h. */
    UndoStore* undo;

    /* The pending session write, and the guard that keeps putting a saved view
     * back from counting as a change to it. */
    guint    session_source;
    gboolean restoring_session;

    /* Composition mode, and what to put back when it ends. */
    gboolean composing;
    gboolean binder_shown_before_composing;
    gboolean inspector_shown_before_composing;
    gboolean format_bar_shown_before_composing;
} WordsmithUiState;

WordsmithUiState* ui_state_new(void);
void               ui_state_free(WordsmithUiState* state);

/** Take ownership of `project`, closing whatever was open, and refresh the
 *  binder, editor and title. Pass NULL to close without opening anything. */
void ui_state_set_project(WordsmithUiState* state, WordsmithProject* project);

/** Rescan the manuscript folder and rebuild the binder. */
void ui_state_reload_project(WordsmithUiState* state);

/** Retitle the window from the project, open document and modified flag. */
void ui_state_update_title(WordsmithUiState* state);

/** Show a side pane or the format bar, or put it away, and remember it. The
 *  View menu's check mark is moved to match, so these are the one way to change
 *  what is on screen: the menu items go through them too, rather than the two
 *  setting each other.
 *
 *  Harmless with no project open — there is simply nowhere to write it down,
 *  and the thing comes back visible next launch. */
void ui_state_set_binder_visible(WordsmithUiState* state, gboolean visible);
void ui_state_set_inspector_visible(WordsmithUiState* state, gboolean visible);
void ui_state_set_format_bar_visible(WordsmithUiState* state, gboolean visible);

/** Mark misspelled words in the editor, or stop marking them, and remember it.
 *
 *  The one way the answer changes, for the reason the three above are: the
 *  editor and the Edit menu's check mark both follow from here, rather than
 *  from each other. Unlike them this is a preference and not session state — it
 *  describes the author rather than the project — so it is written to the
 *  config file and applies to every project they open. */
void ui_state_set_spell_check(WordsmithUiState* state, gboolean enabled);

/** Let a typed `- ` or `1. ` make a list, or stop it, and remember it.
 *
 *  The same shape and the same kind of answer as the line above: a preference,
 *  written to the config file, and the one way it changes. Nothing on screen
 *  moves — what this decides is what the *next* keystroke means — so unlike the
 *  spelling there is nothing to tell the editor about, only the check mark and
 *  the file. */
void ui_state_set_autoformat_lists(WordsmithUiState* state, gboolean enabled);

/* Composition mode: the manuscript alone, full screen.
 *
 * It is a mode rather than a preference, so nothing about it is written down —
 * it ends with the sitting. Both side panes, the format bar and the menu bar go
 * away, and the editor draws its text as a centred column.
 *
 * They are hidden *underneath* the author's standing answer rather than by
 * changing it: the inspector's answer stays in the session and its check mark
 * stays where they left it, unreachable behind a hidden menu bar, and
 * everything comes back the way it was on the way out. Composition mode
 * therefore calls the panels directly and never ui_state_set_inspector_visible(),
 * which would write "dismissed" into the session on the way in. */

/** Enter or leave composition mode. Toggling to the mode already in force does
 *  nothing, so the panes to restore are only ever captured once. */
void ui_state_set_composition_mode(WordsmithUiState* state, gboolean composing);

/** Whether composition mode is in force. */
gboolean ui_state_in_composition_mode(WordsmithUiState* state);

/* Which folders are open in the binder, which document is being edited, and
 * which side panes are on screen are remembered per project, outside it, and
 * put back by ui_state_set_project(). See core/session.hpp for where that lands
 * and why a failure is silent. */

/** Note that the view has changed, and write it out shortly. Called on every
 *  expander click and every document opened, so the delay is what keeps a run
 *  of them to a single write. */
void ui_state_remember_session(WordsmithUiState* state);

/** Write the view out now, ahead of something that is about to take the project
 *  away. Harmless with no project open. */
void ui_state_flush_session(WordsmithUiState* state);

/* Undo and redo.
 *
 * A press addresses the history of the item the author is looking at — the
 * binder's selection — so it can never take back something in a document they
 * had stopped thinking about. Text and formatting go to the editor; a metadata
 * record goes to project-actions.c, which knows the ordering a write needs
 * against the editor's own copy of the frontmatter. */

void ui_state_undo(WordsmithUiState* state);
void ui_state_redo(WordsmithUiState* state);

/** Bring the Edit menu into line with what a press would now take back. Called
 *  after anything that changes a history, including selecting a different item:
 *  the menu reports what is in force rather than deciding it, the same way the
 *  format bar does. */
void ui_state_undo_changed(WordsmithUiState* state);

/** Show `message` and, when non-NULL, `detail`. Takes ownership of `detail`,
 *  freeing it with wordsmith_free_string(), so it pairs with the core's
 *  error out-parameters. */
void ui_state_report_error(WordsmithUiState* state, const char* message,
                           char* detail);
