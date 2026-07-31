#pragma once

#include "ui-state.h"

#include <gtk/gtk.h>

typedef struct MenuBar MenuBar;

/* Builds the application menu model and installs the window's actions. The
 * actions are all stubs for now, so every item is present but inert. */
MenuBar* menu_bar_new(WordsmithUiState* state, GtkApplication* app);
void     menu_bar_free(MenuBar* menu_bar);
