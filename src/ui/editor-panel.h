#pragma once

#include "undo-stack.h"

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

/* Which block a line is. One line is one block, so this is a single answer per
 * line rather than a set of flags — asking for a heading is not adding
 * something to a paragraph, it is saying the line is a heading instead.
 *
 * These are the kinds the author can *reach*. The buffer carries more than
 * this: headings 4 to 6 and code blocks both load, save and round-trip, and
 * neither is offered. A line wearing one of those reads back as
 * EDITOR_BLOCK_OTHER, which the dropdown shows as no answer at all rather than
 * as a wrong one, and a pick passes over it rather than taking it.
 *
 * Left alone rather than taken over because of what undo would owe: a record
 * says what each line was as an EditorBlockStyle, so a line that was a code
 * block has no way to say so, and taking it over would be a change no press
 * could put back. A kind that cannot be restored is worse to offer a way out
 * of than to leave where it is. Making these reachable is the same work in
 * both directions — a style to offer, and a style a record can name. */
typedef enum EditorBlockStyle {
    EDITOR_BLOCK_OTHER = -1,   /* a kind the buffer carries and the UI does not */
    EDITOR_BLOCK_PARAGRAPH = 0,
    EDITOR_BLOCK_HEADING_1,
    EDITOR_BLOCK_HEADING_2,
    EDITOR_BLOCK_HEADING_3,
    EDITOR_BLOCK_QUOTE,
    EDITOR_BLOCK_BULLET_LIST,
    EDITOR_BLOCK_NUMBERED_LIST,
    EDITOR_BLOCK_STYLE_COUNT,
} EditorBlockStyle;

/* Fired when the block the cursor stands in changes, so the format bar's
 * dropdown follows the text the way its buttons do. EDITOR_BLOCK_OTHER when
 * the line is a kind the UI does not offer, and when nothing is open. */
typedef void (*EditorBlockFn)(EditorBlockStyle style, void* user_data);

/* Fired when the open document's undo history changes, so whoever is showing it
 * — the Edit menu — never has to poll. */
typedef void (*EditorHistoryFn)(void* user_data);

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

/** Watch the block the cursor stands in. Fires alongside the styles callback,
 *  off the same cursor moves, for the same reason: what a line is, is the
 *  editor's own reading of its tags, and only one place should be reading. */
void editor_panel_set_block_callback(EditorPanel* editor,
                                     EditorBlockFn callback,
                                     void* user_data);

/** Record every edit into `store`, keyed by the open document's path. Borrowed,
 *  and outlives the document: that is what lets a history survive switching
 *  away and back.
 *
 *  The panel writes into the store directly rather than reporting edits upwards
 *  the way it reports styles, because when a run of typing ends is something
 *  only the panel knows — it turns on the cursor moving, not on anything a
 *  window could see. What it does report is that the history changed, which is
 *  all the menu needs. */
void editor_panel_set_undo_store(EditorPanel* editor, UndoStore* store);

void editor_panel_set_history_callback(EditorPanel* editor,
                                       EditorHistoryFn callback,
                                       void* user_data);

/** Put one record into or out of the buffer; `reverse` is the undo direction.
 *  The edit this makes is not itself recorded, or a press would only ever undo
 *  the press before it. Records of a kind the buffer has nothing to do with —
 *  metadata — are ignored, and belong to project-actions.c. */
void editor_panel_apply_record(EditorPanel* editor, const UndoRecord* record,
                               gboolean reverse);

/** Mark misspelled words, or stop marking them. The panel starts wherever the
 *  preference stands, so this is for the author changing their mind; see
 *  spell-check.h for where the answer is kept and what is left unmarked. */
void editor_panel_set_spell_check(EditorPanel* editor, gboolean enabled);

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

/* ── block styling ───────────────────────────────────────────────────────── */

/** Make every line the selection touches — or the cursor's line, with nothing
 *  selected — a block of `style`.
 *
 *  Unlike an inline style this has nothing to wait for: the kinds are
 *  exclusive, so asking for one is always an answer.
 *
 *  **The two list styles have an "off" and the other five do not.** Asking for
 *  a list where one is already in force across the whole of what is addressed
 *  turns those lines back into paragraphs, exactly as pressing Bold over bold
 *  text does — because a list is the one kind with a *button*, and a lit button
 *  has to mean "click to take this off". A paragraph becomes a list and stops
 *  being one; it does not stop being a heading, it becomes something else
 *  instead. The other five are reached from a dropdown, where "the same again"
 *  is not a gesture that could mean off, so asking for the kind a line already
 *  has does nothing and Paragraph is the way back.
 *
 *  Lines already of that kind, and lines of a kind the UI does not offer, are
 *  passed over — so a pick across a code block styles the prose around it and
 *  leaves it be, and a pick that changes nothing leaves no undo record and no
 *  `*` in the title bar.
 *
 *  Does nothing when no document is open. */
void editor_panel_set_block_style(EditorPanel* editor, EditorBlockStyle style);

/** What one press of `style` means where `in_force` is the kind covering every
 *  line it addresses (EDITOR_BLOCK_OTHER when they are not all one kind).
 *
 *  The whole of the "off" rule above, split out from applying it so it can be
 *  checked without a display — the same seam `editor_ask_for_style()` is for
 *  the inline half, and the same shape: a press folded against what is in
 *  force. */
EditorBlockStyle editor_block_style_for_press(EditorBlockStyle style,
                                              EditorBlockStyle in_force);

/** The block the cursor stands in. Over a selection spanning two kinds, and
 *  over a kind the UI does not offer, EDITOR_BLOCK_OTHER — the same rule the
 *  inline report follows, where a style is in force only if it covers the
 *  whole of what is selected. */
EditorBlockStyle editor_panel_block_style_at_cursor(EditorPanel* editor);

/** What this style is called in a menu ("Heading 1"), and the name the action
 *  takes as its parameter ("heading-1").
 *
 *  One table, because four places have to agree on it: the dropdown, the
 *  Format menu, the accelerators, and the word Edit ▸ Undo uses. Both return
 *  NULL for EDITOR_BLOCK_OTHER and anything out of range; neither is owned. */
const char* editor_block_style_name(EditorBlockStyle style);
const char* editor_block_style_id(EditorBlockStyle style);

/** The style `id` names, or EDITOR_BLOCK_OTHER if it names none. */
EditorBlockStyle editor_block_style_from_id(const char* id);

/* The tags a buffer carries block styling in, gathered so the rules below can
 * be driven without an EditorPanel — a GtkTextBuffer and its tags are plain
 * objects, and this is the same seam editor_style_flags() is.
 *
 * `styles` is indexed by EditorBlockStyle, so `styles[EDITOR_BLOCK_PARAGRAPH]`
 * is NULL: a paragraph is the absence of the others, not a tag of its own. */
typedef struct EditorBlockTags {
    GtkTextTag* styles[EDITOR_BLOCK_STYLE_COUNT];

    /* The kinds the buffer carries and the UI does not offer — a code block,
     * and headings 4 to 6. Read, so a line wearing one answers
     * EDITOR_BLOCK_OTHER instead of passing for a paragraph, and cleared when
     * something offered takes the line over. NULL-terminated. */
    GtkTextTag* unoffered[8];

    GtkTextTag* marker;   /* list markers: shown, but not part of the text */
} EditorBlockTags;

/** Which block `line` is, read from the tags at its first character.
 *
 *  A block tag covers its line's newline as well as its text, which is what
 *  lets an *empty* line carry one: the newline is the only character there, so
 *  without it "make this empty line a heading and start typing" would have
 *  nowhere to put the answer. */
EditorBlockStyle editor_block_style_at(GtkTextBuffer* buffer,
                                       const EditorBlockTags* tags, int line);

/** Make lines [first_line, last_line] blocks of `style`: clear whatever they
 *  were, apply the new tag, and put a list marker in or take one out.
 *
 *  Never adds or removes a line, which is what lets an undo record address
 *  lines by number. Renumbering is left to the caller, since a run of ordered
 *  items may reach past what was touched. */
void editor_block_apply(GtkTextBuffer* buffer, const EditorBlockTags* tags,
                        int first_line, int last_line, EditorBlockStyle style);

/** Rewrite the markers of every numbered run overlapping [first_line,
 *  last_line], counting from 1 and reaching out to each run's real ends.
 *
 *  What the file says is regenerated on save either way; this is so the author
 *  reads 1, 2, 3 rather than 1, 1, 1 while they work. */
void editor_block_renumber(GtkTextBuffer* buffer, const EditorBlockTags* tags,
                           int first_line, int last_line);

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
 * widget to it.
 *
 * Undo and redo are not here: a history holds metadata edits beside text ones,
 * and applying one of those is not something a text panel can do. Choosing
 * which record a press addresses is ui_state_undo()'s job. */
void editor_panel_cut(EditorPanel* editor);
void editor_panel_copy(EditorPanel* editor);
void editor_panel_paste(EditorPanel* editor);
