#pragma once

#include "ui-state.h"

/* The verbs behind the File and Insert menus. Kept apart from menu-bar.c so
 * the action handlers stay thin and the dialog flows are in one place. */

void project_actions_new_dialog(WordsmithUiState* state);
void project_actions_open_dialog(WordsmithUiState* state);
void project_actions_open_path(WordsmithUiState* state, const char* root);
void project_actions_close(WordsmithUiState* state);

void project_actions_save(WordsmithUiState* state);

/** Prompt for a name, then create a folder in the binder's target folder. */
void project_actions_new_folder(WordsmithUiState* state);

/** Prompt for a title, then create a document and open it. */
void project_actions_new_text(WordsmithUiState* state);

/** Open `path` in the editor, saving the current document first. */
void project_actions_open_document(WordsmithUiState* state, const char* path);
