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

/* Fired when the inline styles in force at the cursor change, as a set of
 * WORDSMITH_MARKUP_SPAN_* flags. The format bar follows the text this way
 * rather than by watching the buffer itself: what a run of characters is
 * wearing is the editor's own reading of its tags, and only one place should be
 * doing that reading. */
typedef void (*EditorStylesFn)(uint32_t flags, void* user_data);

EditorPanel* editor_panel_new(void);
void         editor_panel_free(EditorPanel* editor);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* editor_panel_widget(EditorPanel* editor);

void editor_panel_set_modified_callback(EditorPanel* editor,
                                        EditorModifiedFn callback,
                                        void* user_data);

/** Watch the styles at the cursor. Fires on every cursor move and after any
 *  change this panel makes to the tags, so whoever is showing them never has to
 *  poll. Setting the callback does not fire it; the first report follows the
 *  first movement or the first document opened. */
void editor_panel_set_styles_callback(EditorPanel* editor,
                                      EditorStylesFn callback,
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

/** Toggle one WORDSMITH_MARKUP_SPAN_* style over the selection, or — with
 *  nothing selected — over whatever is typed next.
 *
 *  The second case is what "turn bold on and start writing" is: the style is
 *  asked for at a place and waits there, so it is forgotten the moment the
 *  cursor leaves, and the text carries it as soon as any is typed. Both
 *  directions are sayable, because Ctrl+B at the end of a bold word means stop
 *  rather than start. Does nothing when no document is open. */
void editor_panel_toggle_style(EditorPanel* editor, uint32_t span_flag);

/** The inline styles in force at the cursor, as WORDSMITH_MARKUP_SPAN_* flags:
 *  what the text there wears, or what has been asked for and not yet typed
 *  into. 0 when nothing is open. */
uint32_t editor_panel_styles_at_cursor(EditorPanel* editor);

/** The styles `buffer` is wearing where the author is standing, given the
 *  inline tags in bit order — `inline_tags[n]` is the tag for `1 << n`, which
 *  is the order the WORDSMITH_MARKUP_SPAN_* flags are declared in. A NULL entry
 *  is a style this buffer does not carry.
 *
 *  Two questions, because the answer has two jobs. Over a selection it reports
 *  a style only when the whole selection wears it, which is exactly the rule
 *  editor_panel_toggle_style() applies — so a lit button always means "click to
 *  take this off". With no selection it reports the character *behind* the
 *  cursor, which is the one the author just typed past, falling back to the
 *  character ahead at the start of a line where there is nothing behind.
 *
 *  Split out from the panel so both rules can be checked without a display; a
 *  GtkTextBuffer and its tags are plain objects. */
uint32_t editor_style_flags(GtkTextBuffer* buffer, GtkTextTag* const* inline_tags,
                            int count);

/* Typing into a style that is not in the text yet. GtkTextBuffer gives inserted
 * text the tags covering the spot it goes in, which is a different question
 * from the one an author is asking: at the end of a bold word the tag stops
 * short, so GTK hands back plain text exactly where they meant to carry on. The
 * panel therefore dresses every insertion itself, from these two rules. */

/** What text typed at a spot should come out wearing: the styles `beside` it,
 *  overruled where the author has said otherwise. `asked_mask` is which styles
 *  they have answered for, `asked_flags` is the answer — an "off" has to be as
 *  sayable as an "on", which one word of bits could not manage. */
uint32_t editor_typed_styles(uint32_t beside, uint32_t asked_mask,
                             uint32_t asked_flags);

/** Fold one more toggle of `span_flag` into that pair, against what is
 *  `in_force` where the cursor stands: pressing Bold where nothing is bold asks
 *  for it, and pressing it where everything is asks for the end of it. */
void editor_ask_for_style(uint32_t span_flag, uint32_t in_force,
                          uint32_t* asked_mask, uint32_t* asked_flags);

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
