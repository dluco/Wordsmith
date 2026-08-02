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
 *   - The word the cursor is standing in. Without this, every word flashes red
 *     while it is being typed, which is the fastest way to make an author turn
 *     the feature off. It is marked as soon as they leave it.
 *   - Anything wearing a tag handed to spell_check_skip_tag() — the editor
 *     gives it the two code tags. A dictionary has no opinion about
 *     `strcmp`, and marking code as prose is noise.
 *   - Everything, when there is no dictionary installed. The checker answers
 *     that it knows every word, so the manuscript comes up unmarked rather than
 *     solid red, and nothing anywhere reports an error over it.
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

/** Whether the cursor at `cursor` stands in the word spanning [`from`, `to`) —
 *  the word being typed, which is not marked until it is left.
 *
 *  The trailing edge counts and the leading one does not, which is the whole of
 *  the rule and is not symmetry for its own sake. The cursor sits at the *end*
 *  of a word for as long as it takes to type one, so that edge has to be inside
 *  or every word would flash red as it was written. It sits at the *start* of a
 *  word without anything having been typed there — after a click, and after a
 *  document is opened, when it lands at the top of the manuscript. Counting
 *  that edge too would leave the first word of every document as the one word
 *  never checked.
 *
 *  Offsets are characters, the unit GtkTextIter counts in. Split out from the
 *  pass that applies it so the rule can be checked without a display, the way
 *  editor_composition_margin() is. */
gboolean spell_check_cursor_in_word(int cursor, int from, int to);
