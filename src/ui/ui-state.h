#pragma once

#include "core/project-c.h"

#include <gtk/gtk.h>

typedef struct BinderPanel    BinderPanel;
typedef struct EditorPanel    EditorPanel;
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
    EditorPanel*    editor;
    InspectorPanel* inspector;

    /* The pending session write, and the guard that keeps putting a saved view
     * back from counting as a change to it. */
    guint    session_source;
    gboolean restoring_session;

    /* Composition mode, and the panes to put back when it ends. */
    gboolean composing;
    gboolean binder_shown_before_composing;
    gboolean inspector_shown_before_composing;
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

/** Show a side pane, or put it away, and remember it. The View menu's check
 *  mark is moved to match, so these are the one way to change a pane: the menu
 *  items go through them too, rather than the two setting each other.
 *
 *  Harmless with no project open — there is simply nowhere to write it down,
 *  and the pane comes back visible next launch. */
void ui_state_set_binder_visible(WordsmithUiState* state, gboolean visible);
void ui_state_set_inspector_visible(WordsmithUiState* state, gboolean visible);

/* Composition mode: the manuscript alone, full screen.
 *
 * It is a mode rather than a preference, so nothing about it is written down —
 * it ends with the sitting. Both side panes and the menu bar go away, and the
 * editor draws its text as a centred column.
 *
 * The panes are hidden *underneath* the author's standing answer rather than by
 * changing it: the inspector's answer stays in the session and its check mark
 * stays where they left it, unreachable behind a hidden menu bar, and both
 * panes come back the way they were on the way out. Composition mode therefore
 * calls the panels directly and never ui_state_set_inspector_visible(), which
 * would write "dismissed" into the session on the way in. */

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

/** Show `message` and, when non-NULL, `detail`. Takes ownership of `detail`,
 *  freeing it with wordsmith_free_string(), so it pairs with the core's
 *  error out-parameters. */
void ui_state_report_error(WordsmithUiState* state, const char* message,
                           char* detail);
