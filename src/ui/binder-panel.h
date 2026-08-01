#pragma once

#include "core/project-c.h"

#include <gtk/gtk.h>

/* The binder: the left-hand tree of the project's folders and documents.
 *
 * The tree mirrors the manuscript folder on disk. Node handles from the core
 * do not survive a reload, so the whole model is rebuilt each time rather than
 * patched.
 *
 * Right-clicking raises a context menu of win.new-text-in, win.new-folder-in
 * and win.new-folder-with-selection, each carrying the path it applies to as
 * its action target. Those actions are installed by menu-bar.c; the panel only
 * names them, which is what keeps it from having to know about the window.
 *
 * Rows can be dragged to rearrange the manuscript, and renamed in place. In both
 * cases the panel reports what the gesture asked for and leaves the doing to its
 * owner — it never touches the project itself, and rebuilds from whatever is
 * left on disk. */
typedef struct BinderPanel BinderPanel;

/* Fired when the selection changes. `path` is NULL when nothing is selected. */
typedef void (*BinderSelectFn)(const char* path, int is_folder, void* user_data);

/* Where a drop landed relative to the row it was made on. A folder gives up its
 * middle to `INTO`; a document, having no inside, is split down the middle. */
typedef enum BinderDropZone {
    BINDER_DROP_BEFORE,
    BINDER_DROP_INTO,
    BINDER_DROP_AFTER
} BinderDropZone;

/* Fired when a row is dragged onto `target_path`. For BEFORE and AFTER the
 * target is the row the item should sit next to; for INTO it is the folder the
 * item should go in. The panel does not touch the project itself — it rebuilds
 * from whatever the handler leaves on disk. */
typedef void (*BinderMoveFn)(const char* source_path, const char* target_path,
                             BinderDropZone zone, void* user_data);

/* Fired when the set of twisted-open folders changes. The panel does not know
 * what that is worth, only that it happened; whoever does can ask for the list
 * below. */
typedef void (*BinderExpandFn)(void* user_data);

/* Fired when a row's name has been edited in place and accepted. `new_name` is
 * what the author typed, not a filename: the core sanitises it the same way it
 * sanitises the name of anything created. The panel does not touch the project
 * itself — it rebuilds from whatever the handler leaves on disk. */
typedef void (*BinderRenameFn)(const char* path, const char* new_name,
                               void* user_data);

BinderPanel* binder_panel_new(void);
void         binder_panel_free(BinderPanel* binder);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* binder_panel_widget(BinderPanel* binder);

/** Put the pane on screen, or away. Hiding it hides its side of the GtkPaned
 *  holding it, so the rest of the window takes the width and the divider goes
 *  with it; GtkPaned keeps the divider's position while a child is hidden.
 *
 *  A hidden binder keeps its model and its selection. Composition mode is what
 *  uses this today; the View menu's Show Binder is still a stub. */
void binder_panel_set_visible(BinderPanel* binder, gboolean visible);

/** Whether the pane is on screen. */
gboolean binder_panel_is_visible(BinderPanel* binder);

void binder_panel_set_select_callback(BinderPanel* binder,
                                      BinderSelectFn callback,
                                      void* user_data);

void binder_panel_set_move_callback(BinderPanel* binder, BinderMoveFn callback,
                                    void* user_data);

void binder_panel_set_expand_callback(BinderPanel* binder, BinderExpandFn callback,
                                      void* user_data);

void binder_panel_set_rename_callback(BinderPanel* binder, BinderRenameFn callback,
                                      void* user_data);

/** Open an entry over the name of the row at `path` and put the cursor in it.
 *
 *  Renaming happens in the binder rather than in a dialog because the name being
 *  changed is the one on screen, and the row is where the author is already
 *  looking. A click on the name of a row that was *already* selected starts the
 *  same edit — the macOS gesture — which is why this is only the other way in
 *  (F2, and the context menu) rather than the only one.
 *
 *  A row that has scrolled out of the tree has no widget to edit, and this does
 *  nothing rather than scrolling the binder around underneath the author. Both
 *  callers name a row that is on screen: one is the selection, the other was
 *  just right-clicked. */
void binder_panel_begin_rename(BinderPanel* binder, const char* path);

/** Point the binder at a project, or NULL to empty it. The project is borrowed
 *  and must outlive the panel's use of it. Rebuilds the tree. */
void binder_panel_set_project(BinderPanel* binder, WordsmithProject* project);

/** Rebuild the tree from the project's current binder. Call after reloading
 *  the project. */
void binder_panel_reload(BinderPanel* binder);

/** The selected item, folder or document, or NULL when nothing is selected or
 *  no project is open. Caller frees with g_free().
 *
 *  This is what an undo addresses: a history belongs to the item the author is
 *  looking at, and the binder is the one place that knows which that is. */
char* binder_panel_selected_path(BinderPanel* binder);

/** Where a newly created folder or document should go: the selected folder,
 *  the selected document's parent, or the manuscript root when nothing is
 *  selected. NULL when no project is open. Caller frees with g_free(). */
char* binder_panel_target_folder(BinderPanel* binder);

/** Select the document at `path` if it is in the tree, expanding ancestors as
 *  needed. */
void binder_panel_select_path(BinderPanel* binder, const char* path);

/** The folders currently twisted open, as absolute paths, NULL-terminated.
 *  Never NULL. Caller frees with g_strfreev().
 *
 *  A folder inside a collapsed one is not listed: it is not on screen, and the
 *  point of the list is to put back what was. */
char** binder_panel_expanded_paths(BinderPanel* binder);

/** Twist open the folders named in `paths`, a NULL-terminated array of absolute
 *  paths, leaving the rest as they are. Names with no folder behind them are
 *  ignored, so a list that has drifted gives a partly-opened binder rather than
 *  a failure. Does not fire the expand callback. */
void binder_panel_set_expanded_paths(BinderPanel* binder, const char* const* paths);
