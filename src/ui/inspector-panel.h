#pragma once

#include <gtk/gtk.h>

/* The inspector: the right-hand pane of per-document metadata.
 *
 * For a document, what it shows comes from the YAML frontmatter, split off by
 * core/frontmatter-c.h before the editor sees the file. For a folder, which has
 * no file of its own to carry frontmatter, it comes from the metadata.yaml
 * sidecar inside it. Fields the schema knows about (title, synopsis, status,
 * tags, and the rest of SCHEMA_FIELDS) get their own labelled row; every other
 * key is still listed, in the order the file wrote it, so nothing is invisible
 * here.
 *
 * Read-only for now. The core's set_field() is the seam for editing: it
 * rewrites one field's bytes and leaves the rest of the file alone. */
typedef struct InspectorPanel InspectorPanel;

InspectorPanel* inspector_panel_new(void);
void            inspector_panel_free(InspectorPanel* inspector);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* inspector_panel_widget(InspectorPanel* inspector);

/** Show the frontmatter of the document at `path`. Reading it is cheap and
 *  keeps the inspector honest about what is on disk rather than what the editor
 *  buffer holds. A file that cannot be read clears the pane. */
void inspector_panel_set_document(InspectorPanel* inspector, const char* path);

/** Show the fields of the folder at `path`, read from its metadata.yaml. The
 *  recorded child order is left out: the binder already draws it. */
void inspector_panel_set_folder(InspectorPanel* inspector, const char* path);

/** Return to the empty state. */
void inspector_panel_clear(InspectorPanel* inspector);
