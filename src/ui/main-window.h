#pragma once

#include <gtk/gtk.h>

/* Builds and shows the main project window: binder, editor and inspector
 * under a menu bar. `initial_project`, when non-NULL, names a project
 * directory to open once the window is up. */
void main_window_present(GtkApplication* app, const char* initial_project);
