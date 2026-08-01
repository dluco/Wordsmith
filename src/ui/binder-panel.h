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
 * Rows can be dragged to rearrange the manuscript. The panel reports where the
 * drop landed and leaves the moving to its owner. */
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

BinderPanel* binder_panel_new(void);
void         binder_panel_free(BinderPanel* binder);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* binder_panel_widget(BinderPanel* binder);

void binder_panel_set_select_callback(BinderPanel* binder,
                                      BinderSelectFn callback,
                                      void* user_data);

void binder_panel_set_move_callback(BinderPanel* binder, BinderMoveFn callback,
                                    void* user_data);

/** Point the binder at a project, or NULL to empty it. The project is borrowed
 *  and must outlive the panel's use of it. Rebuilds the tree. */
void binder_panel_set_project(BinderPanel* binder, WordsmithProject* project);

/** Rebuild the tree from the project's current binder. Call after reloading
 *  the project. */
void binder_panel_reload(BinderPanel* binder);

/** Where a newly created folder or document should go: the selected folder,
 *  the selected document's parent, or the manuscript root when nothing is
 *  selected. NULL when no project is open. Caller frees with g_free(). */
char* binder_panel_target_folder(BinderPanel* binder);

/** Select the document at `path` if it is in the tree, expanding ancestors as
 *  needed. */
void binder_panel_select_path(BinderPanel* binder, const char* path);
