#include "ui-state.h"

#include "binder-panel.h"
#include "editor-panel.h"
#include "inspector-panel.h"
#include "menu-bar.h"

WordsworthUiState* ui_state_new(void)
{
    return g_new0(WordsworthUiState, 1);
}

void ui_state_free(WordsworthUiState* state)
{
    if (state == NULL) {
        return;
    }

    menu_bar_free(state->menu_bar);
    binder_panel_free(state->binder);
    editor_panel_free(state->editor);
    inspector_panel_free(state->inspector);

    g_free(state);
}
