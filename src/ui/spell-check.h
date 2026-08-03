#pragma once

#include <gtk/gtk.h>

/* Marking the misspelled words in the manuscript.
 *
 * Two things live here, because they are two halves of one answer: whether the
 * author wants words marked at all, which is an application preference like the
 * text size, and the marking itself, which is a red line under a range of a
 * GtkTextBuffer.
 *
 * ## The answer
 *
 * It is a preference rather than session state — it describes the person, not
 * the project, so it lives in the config file outside every project and every
 * window shows the same answer (see core/preferences.hpp). It is on unless the
 * author has turned it off, and every way of not answering means on.
 *
 * The read and the write are here, in front of core/preferences-c.h, for the
 * reason text-scale.c holds the size: one place has to hold what is in force,
 * or the check mark in the menu and the marks on the page would each be free to
 * believe something different.
 *
 * ## The marking
 *
 * A SpellCheck attaches to one buffer, creates the tag that draws the line, and
 * follows the buffer's own signals from then on. The editor panel does not
 * forward its edits: what a decoration needs to know is exactly what the buffer
 * already says, and routing it through the panel would put a second copy of
 * "which lines changed" in a file that is long enough already. What the panel
 * does say is when it is loading a document, because a load is hundreds of
 * insertions that add up to one document; see spell_check_hold().
 *
 * Rechecking works a line at a time. A line in this buffer is a whole block —
 * a paragraph, a heading — so the unit is bigger than it sounds, and it is
 * still the right one: deleting the space between two words makes one word out
 * of two, and anything finer would have to find that out for itself.
 *
 * ## What is not marked
 *
 * Three things, and each is a false mark avoided rather than a nicety:
 *
 *   - The word being typed. Without this, every word flashes red while it is
 *     being written, which is the fastest way to make an author turn the
 *     feature off. It is marked as soon as they leave it.
 *   - Anything wearing a tag handed to spell_check_skip_tag() — the editor
 *     gives it the two code tags. A dictionary has no opinion about
 *     `strcmp`, and marking code as prose is noise.
 *   - Everything, when there is no dictionary installed. The checker answers
 *     that it knows every word, so the manuscript comes up unmarked rather than
 *     solid red, and nothing anywhere reports an error over it.
 *
 * ## The menu over a misspelling
 *
 * A word with a red line under it is a question, and the answer has to be
 * reachable from the word itself: right-clicking one offers what it might have
 * been, and the two ways of saying it is a word already.
 *
 * **The corrections come first, so the menu is ours.** That one requirement is
 * what decides the whole shape of this. GTK4 gives no way to build the text
 * view's context menu as it opens — GTK3's `populate-popup` is gone and there
 * is no signal in its place — and the one hook it does give,
 * gtk_text_view_set_extra_menu(), joins a model onto the **end** of the view's
 * own (`gtk_joined_menu_append_menu`, and nothing chooses where). A menu whose
 * first item answers the question the author clicked to ask cannot be built
 * that way at all.
 *
 * So the press is **taken** rather than decorated. A secondary-click gesture in
 * the capture phase runs ahead of GtkTextView's own — GTK adds that one in the
 * bubble phase — and when the click lands on a marked word it claims the
 * sequence, so the view never opens its menu, and puts up a popover of ours in
 * its place. Every other press is left alone, so the ordinary context menu is
 * still the ordinary context menu.
 *
 * The price is that a menu of ours has to carry the editing verbs itself, and
 * this file now names Cut, Copy, Paste, Delete, Select All and Insert Emoji.
 * They are GtkTextView's own actions under GtkTextView's own labels, in its
 * order, so the two menus are one menu with a spelling section on top. Two
 * things do not come for free with them:
 *
 *   - **Undo and Redo name `win.undo` and `win.redo`**, not GtkTextView's
 *     `text.undo`. The view's pair drives the buffer's own history, which is
 *     switched off at construction (see undo-stack.h), so naming it would put
 *     two permanently dead items in a menu we are building by hand anyway.
 *   - **Cut, Copy and Paste have to be enabled by hand.** GTK does that in a
 *     static function on its way up, so a menu that names those verbs and does
 *     not answer the same questions offers a Paste that does nothing. That is
 *     update_editing_items(), and it is the part most likely to drift as GTK
 *     changes.
 *
 * The model is filled fresh on every press and the popover is built once from
 * it: GTK tracks a GMenu's `items-changed`, so the menu follows without being
 * rebuilt.
 *
 * A context menu raised from the keyboard — Menu, Shift+F10 — is GtkTextView's
 * own and holds no spelling. Nothing can prepare that one, and the word under
 * the cursor is by then the word being typed, which is the one word that is
 * never marked.
 *
 * The menu names actions in a "spelling" group the marker installs on the view,
 * not window actions. The binder's context menu names window actions because
 * its verbs move files and open dialogs; these three change one word in one
 * buffer, and routing them through the window would put the dictionary in
 * main-window.c's hands to no end.
 *
 * A correction reaches the buffer as an ordinary delete and insert, so the
 * editor's own handlers record it, mark the document modified and recheck the
 * line without being told. It is two records rather than one, so taking it back
 * is two presses of Ctrl+Z; a compound record is the fix, and it belongs to
 * undo-stack.h rather than here.
 */

/* ── the answer ──────────────────────────────────────────────────────────── */

/** Read the saved answer. Call once at startup, beside text_scale_init(). */
void spell_check_init(void);

/** Whether words are to be marked. */
gboolean spell_check_wanted(void);

/** Record whether words are to be marked, and save it as the preference.
 *
 *  The answer is in force whether or not it could be saved. Returns 0 and fills
 *  `error` (owned by the caller, freed with wordsmith_free_string) when the
 *  save failed, which is worth reporting for the reason the text size is: right
 *  now, wrong after a restart. */
int spell_check_set_wanted(gboolean wanted, char** error);

/* ── the marking ─────────────────────────────────────────────────────────── */

typedef struct SpellCheck SpellCheck;

/** Mark misspelled words in `buffer`, from now on. Borrowed: the buffer
 *  outlives this, and freeing it leaves the buffer's text untouched.
 *
 *  Starts in whatever state spell_check_wanted() reports. The dictionary is not
 *  opened until the first word is checked, so an author who has the marking
 *  turned off never pays for one. */
SpellCheck* spell_check_new(GtkTextBuffer* buffer);

void spell_check_free(SpellCheck* spelling);

/** Leave text wearing `tag` unchecked. Borrowed, and expected to outlive this.
 *  Call before anything is checked. */
void spell_check_skip_tag(SpellCheck* spelling, GtkTextTag* tag);

/** Whether a dictionary was found. Nothing needs to change either way — this is
 *  for saying so in a test. */
gboolean spell_check_available(SpellCheck* spelling);

/** Turn marking on or off for this buffer. Turning it off takes every mark off
 *  the page; turning it on puts them back. */
void spell_check_set_enabled(SpellCheck* spelling, gboolean enabled);

/** Stop following the buffer, and start again.
 *
 *  For a wholesale change of the text — loading a document is one — where
 *  following each insertion would mean checking the same lines as many times as
 *  there are blocks. Release rechecks everything once. They nest, so a caller
 *  need not know whether it is inside another hold. */
void spell_check_hold(SpellCheck* spelling);
void spell_check_release(SpellCheck* spelling);

/** Check the whole buffer again, from the top. */
void spell_check_refresh(SpellCheck* spelling);

/** Whether the word spanning [`from`, `to`) is the one being typed, and so the
 *  one word that is not marked: the cursor is standing in it, and it got there
 *  by editing rather than by being put there.
 *
 *  `typed_at` is where the last insertion or deletion left the text, or -1 when
 *  nothing has been edited yet. Requiring the cursor to still be *there* is
 *  what tells typing from arriving: a click into a misspelling is the author
 *  asking about the mark, and taking the mark off under the pointer answers the
 *  question by hiding it. The same goes for an arrow key — nothing has been
 *  written, so nothing is in progress.
 *
 *  Of the two edges of the word, the trailing one counts and the leading one
 *  does not, and that asymmetry is not for its own sake. The cursor sits at the
 *  *end* of a word for as long as it takes to type one, so that edge has to be
 *  inside or every word would flash red as it was written. It sits at the
 *  *start* of one having typed nothing there — deleting the word in front of it
 *  leaves it exactly so — and that word is finished.
 *
 *  Offsets are characters, the unit GtkTextIter counts in. Split out from the
 *  pass that applies it so the rule can be checked without a display, the way
 *  editor_composition_margin() is. */
gboolean spell_check_word_being_typed(int cursor, int typed_at, int from, int to);

/* ── the menu over a misspelling ─────────────────────────────────────────── */

/** Open the corrections for a misspelled word on a secondary click in `view`,
 *  which is expected to be showing this marker's buffer. A reference is held
 *  until spell_check_free(), which takes the gesture, the actions and the
 *  popover back off it — the widget may be gone by then, and a gesture pointing
 *  at freed memory is not. */
void spell_check_attach_menu(SpellCheck* spelling, GtkTextView* view);

/** Fill `menu` with the whole of what a click on `word` opens: what it might
 *  have been, then the two ways of saying it is a word — for this sitting or
 *  for good — and then the view's ordinary editing verbs. Whatever was in it
 *  goes, since the menu outlives the word it was last about.
 *
 *  **The corrections are first.** That is the reason this menu exists rather
 *  than an addition to GtkTextView's, which can only be joined onto at the end.
 *
 *  Long lists of near-identical suggestions are what a dictionary is happy to
 *  produce and no author reads, so only the first few are offered. An empty
 *  list still fills the menu, with one dead item that says so: a right click
 *  that opens nothing looks like a fault, and "no suggestions" is an answer.
 *
 *  The display-free seam, and the reason the model is filled apart from the
 *  gesture that shows it. */
void spell_check_fill_menu(GMenu* menu, const char* word,
                           const char* const* corrections, size_t count);
