#pragma once

#include <gtk/gtk.h>
#include <stdint.h>

/* The centre pane: the manuscript editor.
 *
 * A GtkTextView over a GtkTextBuffer whose tags mirror the Markdown model in
 * core/markup-c.h. Loading parses Markdown into tagged text; saving walks the
 * tags back into Markdown through the core's builder, so escaping and
 * delimiter placement stay testable without a display.
 *
 * The buffer holds one block per line, with two exceptions: a code block's
 * body spans as many lines as it has, and its fences are not in the buffer at
 * all.
 *
 * YAML frontmatter is not in the buffer either. Loading splits it off through
 * core/frontmatter-c.h and keeps the bytes aside; the inspector shows that
 * metadata, and saving puts the original bytes back ahead of the body. */
typedef struct EditorPanel EditorPanel;

/* The ordinary gap between the text and the edge of the pane. */
#define EDITOR_SIDE_MARGIN 48

/* How wide the text column is in composition mode. Sixty-odd characters at the
 * default size, which is where prose stops being a chore to track back across.
 * Fixed for now; if it becomes settable it belongs in preferences.hpp with the
 * text size, since it describes the reader rather than the manuscript. */
#define EDITOR_COMPOSITION_COLUMN 700

/* Fired when the document's modified flag changes, so the window can retitle. */
typedef void (*EditorModifiedFn)(int modified, void* user_data);

EditorPanel* editor_panel_new(void);
void         editor_panel_free(EditorPanel* editor);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* editor_panel_widget(EditorPanel* editor);

void editor_panel_set_modified_callback(EditorPanel* editor,
                                        EditorModifiedFn callback,
                                        void* user_data);

/** Load `path` into the view. Returns 0 and fills `error` (owned by the
 *  caller, freed with wordsmith_free_string) on failure. */
int editor_panel_load(EditorPanel* editor, const char* path, char** error);

/** Write the buffer back to the open document. Returns 0 and fills `error` on
 *  failure. Succeeds trivially when nothing is open. */
int editor_panel_save(EditorPanel* editor, char** error);

/** Re-read the open document's frontmatter from disk, leaving the body in the
 *  buffer alone.
 *
 *  Saving puts the frontmatter bytes back as they were when the document was
 *  loaded, so anything that rewrites those bytes underneath the editor — the
 *  inspector, editing a field — has to say so here, or the next save will put
 *  the old metadata back. Does nothing when nothing is open. */
void editor_panel_refresh_frontmatter(EditorPanel* editor);

/** Drop the open document and clear the view. */
void editor_panel_close(EditorPanel* editor);

/** The open document's path, or NULL when nothing is open. Borrowed. */
const char* editor_panel_path(EditorPanel* editor);

gboolean editor_panel_is_modified(EditorPanel* editor);

/** Toggle one WORDSMITH_MARKUP_SPAN_* style over the selection. Does nothing
 *  when the selection is empty. */
void editor_panel_toggle_style(EditorPanel* editor, uint32_t span_flag);

/* Composition mode's side of the editor: the text draws as a column of fixed
 * width in the middle of the pane, however wide the pane has become.
 *
 * The column is made of the view's own left and right margins rather than a
 * width on the widget, because GTK's CSS has no `max-width` and putting the
 * view in something narrower would mean wrapping it in a viewport — which costs
 * GtkTextView its line-by-line layout, and a novel is exactly the document that
 * cannot afford to be laid out all at once. So the margins are recomputed
 * whenever the pane's width changes. */

/** Draw the text as a centred column, or go back to the ordinary margins. */
void editor_panel_set_composition(EditorPanel* editor, gboolean composing);

/** The side margin that centres a column `column` wide inside a pane `width`
 *  pixels across, never tighter than EDITOR_SIDE_MARGIN — a window too narrow
 *  for the column gets the ordinary margins rather than a negative one.
 *
 *  Split out from applying it so the arithmetic can be checked without a
 *  display, the way text_scale_css() is. */
int editor_composition_margin(int width, int column);

/* Editing verbs behind the Edit menu. The window registers accelerators for
 * these, and an application accelerator outranks GtkTextView's own key
 * bindings, so the menu has to drive the buffer itself rather than leave the
 * widget to it. */
void editor_panel_undo(EditorPanel* editor);
void editor_panel_redo(EditorPanel* editor);
void editor_panel_cut(EditorPanel* editor);
void editor_panel_copy(EditorPanel* editor);
void editor_panel_paste(EditorPanel* editor);
