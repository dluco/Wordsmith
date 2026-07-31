#pragma once

#include <gtk/gtk.h>

typedef struct BinderPanel    BinderPanel;
typedef struct EditorPanel    EditorPanel;
typedef struct InspectorPanel InspectorPanel;
typedef struct MenuBar        MenuBar;

/* Shared per-window state. Panels borrow this and never own it; it is freed
 * when the window it belongs to is destroyed. */
typedef struct WordsworthUiState {
    GtkWindow* window;

    MenuBar*        menu_bar;
    BinderPanel*    binder;
    EditorPanel*    editor;
    InspectorPanel* inspector;
} WordsworthUiState;

WordsworthUiState* ui_state_new(void);
void               ui_state_free(WordsworthUiState* state);
