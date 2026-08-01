#pragma once

#include <gtk/gtk.h>

#include <stddef.h>

/* The inspector: the right-hand pane of per-document metadata.
 *
 * For a document, what it shows comes from the YAML frontmatter, split off by
 * core/frontmatter-c.h before the editor sees the file. For a folder, which has
 * no file of its own to carry frontmatter, it comes from the metadata.yaml
 * sidecar inside it.
 *
 * Title, synopsis, status and tags are always shown and always editable, even
 * for an item whose file carries no metadata at all: a pane that renders only
 * what is already on disk gives an author with an empty document nothing to
 * click on, and the way to add a field is to type in it. Everything else the
 * file holds is shown read-only, spelled as the file spells it — a key we do
 * not recognise is not a key we should hide.
 *
 * Status is chosen from a list, each entry carrying a mark that shows how far
 * along the work is. The list is open at the end: a status the file already
 * holds that this build has never heard of is kept and offered back rather than
 * dropped. Tags are still typed as one comma-separated line, and still want
 * tokens that can be dismissed one at a time.
 *
 * The panel does not write anything itself. A committed edit is reported
 * through InspectorCommitFn, because the file behind it may also be open in the
 * editor, and getting that right means ordering a save against a rewrite —
 * which is project-actions.c's job, not this pane's.
 *
 * The pane can be put away (View ▸ Show Inspector). Hidden it is still loaded
 * and still following the binder's selection, because what it costs to keep up
 * to date is one read of a file already on disk, and coming back to a pane
 * showing the last document visited before it was dismissed would be a lie
 * about what is open. */
typedef struct InspectorPanel InspectorPanel;

/* One committed field edit.
 *
 * `items` non-NULL asks for a sequence, and is how a list-valued field arrives
 * however the pane happened to let the author type it. Otherwise `value` is the
 * new scalar, and both NULL together mean the field should go away: an author
 * who empties a field is removing it, not setting it to nothing. */
typedef struct InspectorEdit {
    const char* path;        /* the document, or the folder whose sidecar it is */
    int         is_folder;
    const char* key;
    const char* value;
    const char* const* items;
    size_t             item_count;
} InspectorEdit;

typedef void (*InspectorCommitFn)(const InspectorEdit* edit, void* user_data);

InspectorPanel* inspector_panel_new(void);
void            inspector_panel_free(InspectorPanel* inspector);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* inspector_panel_widget(InspectorPanel* inspector);

void inspector_panel_set_commit_callback(InspectorPanel* inspector,
                                         InspectorCommitFn callback,
                                         void* user_data);

/** Put the pane on screen, or away. Hiding the pane hides its side of the
 *  GtkPaned holding it: the editor takes the whole width and the divider goes
 *  with it. GtkPaned keeps the divider's position while a child is hidden, so
 *  the pane comes back the width the author last dragged it to. */
void inspector_panel_set_visible(InspectorPanel* inspector, gboolean visible);

/** Whether the pane is on screen. This is what gets written down when the view
 *  is remembered, so it reads the widget rather than a flag beside it: the
 *  widget is the one copy that cannot be out of date. */
gboolean inspector_panel_is_visible(InspectorPanel* inspector);

/** Show the frontmatter of the document at `path`. Reading it is cheap and
 *  keeps the inspector honest about what is on disk rather than what the editor
 *  buffer holds. A file that cannot be read clears the pane. */
void inspector_panel_set_document(InspectorPanel* inspector, const char* path);

/** Show the fields of the folder at `path`, read from its metadata.yaml. A
 *  folder with no sidecar yet shows the same empty fields as one with an empty
 *  sidecar; writing to any of them creates it. The recorded child order is left
 *  out: the binder already draws it. */
void inspector_panel_set_folder(InspectorPanel* inspector, const char* path);

/** Show whatever is on disk for the item the pane is already on. Cheap enough
 *  to call after a write, and does nothing when the pane is empty. */
void inspector_panel_reload(InspectorPanel* inspector);

/** Return to the empty state. */
void inspector_panel_clear(InspectorPanel* inspector);
