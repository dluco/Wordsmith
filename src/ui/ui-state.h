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

/* Which folders are open in the binder and which document is being edited are
 * remembered per project, outside it, and put back by ui_state_set_project().
 * See core/session.hpp for where that lands and why a failure is silent. */

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
