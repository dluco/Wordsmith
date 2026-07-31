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

/** Show `message` and, when non-NULL, `detail`. Takes ownership of `detail`,
 *  freeing it with wordsmith_free_string(), so it pairs with the core's
 *  error out-parameters. */
void ui_state_report_error(WordsmithUiState* state, const char* message,
                           char* detail);
