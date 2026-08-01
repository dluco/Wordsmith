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
 * Status and tags are plain text for now. Both want a control of their own — a
 * chosen-from list, and tokens that can be dismissed — and both are typed as
 * one line here until they get one; tags is comma-separated meanwhile.
 *
 * The panel does not write anything itself. A committed edit is reported
 * through InspectorCommitFn, because the file behind it may also be open in the
 * editor, and getting that right means ordering a save against a rewrite —
 * which is project-actions.c's job, not this pane's. */
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
