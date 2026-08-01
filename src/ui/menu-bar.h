#pragma once

#include "ui-state.h"

#include <gtk/gtk.h>

typedef struct MenuBar MenuBar;

/* Builds the application menu model and installs the window's actions. Several
 * are still stubs, so their items are present but inert. */
MenuBar* menu_bar_new(WordsmithUiState* state, GtkApplication* app);
void     menu_bar_free(MenuBar* menu_bar);

/** Name what Undo and Redo would take back — "Typing", "Bold", "Synopsis" — and
 *  grey out either one that has nothing behind it. NULL means nothing to take
 *  back in that direction.
 *
 *  The menu reports what is in force rather than deciding it, the same way the
 *  format bar does: ui-state calls this after anything that changes a history,
 *  including selecting a different item. */
void menu_bar_show_undo(MenuBar* menu_bar, const char* undo_verb,
                        const char* redo_verb);
