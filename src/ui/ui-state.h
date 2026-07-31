#pragma once

#include "core/project-c.h"

#include <gtk/gtk.h>

typedef struct BinderPanel    BinderPanel;
typedef struct EditorPanel    EditorPanel;
typedef struct InspectorPanel InspectorPanel;
typedef struct MenuBar        MenuBar;

/* Shared per-window state. Panels borrow this and never own it; it is freed
 * when the window it belongs to is destroyed. */
typedef struct WordsworthUiState {
    GtkWindow*      window;
    GtkApplication* app;      /* borrowed */

    WordsworthProject* project;   /* owned; NULL when no project is open */

    MenuBar*        menu_bar;
    BinderPanel*    binder;
    EditorPanel*    editor;
    InspectorPanel* inspector;
} WordsworthUiState;

WordsworthUiState* ui_state_new(void);
void               ui_state_free(WordsworthUiState* state);

/** Take ownership of `project`, closing whatever was open, and refresh the
 *  binder, editor and title. Pass NULL to close without opening anything. */
void ui_state_set_project(WordsworthUiState* state, WordsworthProject* project);

/** Rescan the manuscript folder and rebuild the binder. */
void ui_state_reload_project(WordsworthUiState* state);

/** Retitle the window from the project, open document and modified flag. */
void ui_state_update_title(WordsworthUiState* state);

/** Show `message` and, when non-NULL, `detail`. Takes ownership of `detail`,
 *  freeing it with wordsworth_free_string(), so it pairs with the core's
 *  error out-parameters. */
void ui_state_report_error(WordsworthUiState* state, const char* message,
                           char* detail);
