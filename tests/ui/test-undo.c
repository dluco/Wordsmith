#include "ui/editor-panel.h"
#include "ui/undo-stack.h"

#include <gtk/gtk.h>

/* The undo history, driven the way the editor drives it.
 *
 * A GtkTextBuffer and its tags are plain objects, so everything here — the
 * records, the styling they carry, the coalescing rule and the store — can be
 * checked without a display. What is deliberately not here is the editor
 * panel's signal wiring, which needs a realised widget; the seam between the
 * two is undo_record_capture_text() and undo_record_apply(), and those are what
 * this file drives.
 *
 * The tags are the editor's four, in bit order. */
#define TAG_COUNT 4
#define TAG_EMPHASIS 0
#define TAG_STRONG   1

typedef struct Fixture {
    GtkTextBuffer* buffer;
    GtkTextTag*    tags[TAG_COUNT];
} Fixture;

static void fixture_init(Fixture* fixture, const char* text)
{
    fixture->buffer  = gtk_text_buffer_new(NULL);
    fixture->tags[0] = gtk_text_buffer_create_tag(fixture->buffer, "emphasis",
                                                  "style", PANGO_STYLE_ITALIC, NULL);
    fixture->tags[1] = gtk_text_buffer_create_tag(fixture->buffer, "strong",
                                                  "weight", PANGO_WEIGHT_BOLD, NULL);
    fixture->tags[2] = gtk_text_buffer_create_tag(fixture->buffer, "underline",
                                                  "underline", PANGO_UNDERLINE_SINGLE,
                                                  NULL);
    fixture->tags[3] = gtk_text_buffer_create_tag(fixture->buffer, "code-span",
                                                  "family", "monospace", NULL);
    gtk_text_buffer_set_text(fixture->buffer, text, -1);
}

static void fixture_clear(Fixture* fixture)
{
    g_object_unref(fixture->buffer);
}

static void apply_tag(Fixture* fixture, int bit, int from, int to)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &start, from);
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &end, to);
    gtk_text_buffer_apply_tag(fixture->buffer, fixture->tags[bit], &start, &end);
}

static gboolean has_tag_at(Fixture* fixture, int bit, int offset)
{
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &at, offset);
    return gtk_text_iter_has_tag(&at, fixture->tags[bit]);
}

static char* buffer_text(Fixture* fixture)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(fixture->buffer, &start, &end);
    return gtk_text_buffer_get_text(fixture->buffer, &start, &end, TRUE);
}

static void assert_text(Fixture* fixture, const char* expected)
{
    char* actual = buffer_text(fixture);
    g_assert_cmpstr(actual, ==, expected);
    g_free(actual);
}

/* A record standing for one insertion, without a buffer behind it. The rule
 * only reads offsets and text, which is the point of splitting it out. */
static UndoRecord* text_record(UndoKind kind, int from, const char* text)
{
    UndoRecord* record = g_new0(UndoRecord, 1);
    record->kind       = kind;
    record->text.from  = from;
    record->text.text  = g_strdup(text);
    return record;
}

/* ── the coalescing rule ─────────────────────────────────────────────────── */

/* A run of characters typed one after another is one thing done. Without this
 * Ctrl+Z takes back a letter, which is no use to anyone writing prose. */
static void test_typing_a_word_is_one_entry(void)
{
    UndoRecord* h = text_record(UNDO_TEXT_INSERT, 0, "h");
    UndoRecord* e = text_record(UNDO_TEXT_INSERT, 1, "e");
    UndoRecord* l = text_record(UNDO_TEXT_INSERT, 2, "l");

    g_assert_true(undo_records_coalesce(h, e));
    g_assert_true(undo_records_coalesce(e, l));

    /* Not contiguous: the author typed somewhere else. */
    UndoRecord* elsewhere = text_record(UNDO_TEXT_INSERT, 40, "x");
    g_assert_false(undo_records_coalesce(l, elsewhere));

    undo_record_free(h);
    undo_record_free(e);
    undo_record_free(l);
    undo_record_free(elsewhere);
}

/* The break falls where a word does. A space joins the word it follows, so
 * "hello " comes back in one press and "world" is the next entry rather than
 * the space being an entry of its own. */
static void test_a_run_breaks_at_the_next_word(void)
{
    UndoRecord* word  = text_record(UNDO_TEXT_INSERT, 0, "hello");
    UndoRecord* space = text_record(UNDO_TEXT_INSERT, 5, " ");
    g_assert_true(undo_records_coalesce(word, space));

    UndoRecord* run  = text_record(UNDO_TEXT_INSERT, 0, "hello ");
    UndoRecord* next = text_record(UNDO_TEXT_INSERT, 6, "w");
    g_assert_false(undo_records_coalesce(run, next));

    undo_record_free(word);
    undo_record_free(space);
    undo_record_free(run);
    undo_record_free(next);
}

/* A newline is a place the author stopped, whichever side of the join it is
 * on. */
static void test_a_newline_ends_a_run(void)
{
    UndoRecord* line    = text_record(UNDO_TEXT_INSERT, 0, "hello");
    UndoRecord* newline = text_record(UNDO_TEXT_INSERT, 5, "\n");
    g_assert_false(undo_records_coalesce(line, newline));

    UndoRecord* after = text_record(UNDO_TEXT_INSERT, 0, "hello\n");
    UndoRecord* more  = text_record(UNDO_TEXT_INSERT, 6, "w");
    g_assert_false(undo_records_coalesce(after, more));

    undo_record_free(line);
    undo_record_free(newline);
    undo_record_free(after);
    undo_record_free(more);
}

/* Backspacing walks left and forward-delete stays put, so the two read as runs
 * in opposite directions — and never as the same run, because changing
 * direction is changing your mind. */
static void test_deletions_coalesce_in_both_directions(void)
{
    UndoRecord* first  = text_record(UNDO_TEXT_DELETE, 5, "d");
    UndoRecord* second = text_record(UNDO_TEXT_DELETE, 4, "l");
    g_assert_true(undo_records_coalesce(first, second));

    UndoRecord* ahead   = text_record(UNDO_TEXT_DELETE, 5, "d");
    UndoRecord* ahead_2 = text_record(UNDO_TEXT_DELETE, 5, "e");
    g_assert_true(undo_records_coalesce(ahead, ahead_2));

    /* An insertion and a deletion are never one run. */
    UndoRecord* typed = text_record(UNDO_TEXT_INSERT, 5, "d");
    g_assert_false(undo_records_coalesce(typed, second));

    undo_record_free(first);
    undo_record_free(second);
    undo_record_free(ahead);
    undo_record_free(ahead_2);
    undo_record_free(typed);
}

/* ── records against a buffer ────────────────────────────────────────────── */

/* Undoing an insertion takes the text away; redoing it puts the text back
 * *wearing what it wore*. Dropping the styling would launder formatting out of
 * the manuscript one press at a time. */
static void test_redoing_an_insertion_restores_its_styling(void)
{
    Fixture fixture;
    fixture_init(&fixture, "plain bold");
    apply_tag(&fixture, TAG_STRONG, 6, 10);

    UndoRecord* record = undo_record_capture_text(
        fixture.buffer, fixture.tags, TAG_COUNT, UNDO_TEXT_INSERT, 6, 10);
    g_assert_nonnull(record);

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, TRUE);
    assert_text(&fixture, "plain ");

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, FALSE);
    assert_text(&fixture, "plain bold");
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 6));
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 9));
    g_assert_false(has_tag_at(&fixture, TAG_STRONG, 0));

    undo_record_free(record);
    fixture_clear(&fixture);
}

/* The same the other way round: deleting bold text and taking the deletion back
 * has to give bold text back, not a plain copy of the characters. */
static void test_undoing_a_deletion_restores_bold_as_bold(void)
{
    Fixture fixture;
    fixture_init(&fixture, "a bold word");
    apply_tag(&fixture, TAG_STRONG, 2, 6);

    UndoRecord* record = undo_record_capture_text(
        fixture.buffer, fixture.tags, TAG_COUNT, UNDO_TEXT_DELETE, 2, 7);

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(fixture.buffer, &start, 2);
    gtk_text_buffer_get_iter_at_offset(fixture.buffer, &end, 7);
    gtk_text_buffer_delete(fixture.buffer, &start, &end);
    assert_text(&fixture, "a word");

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, TRUE);
    assert_text(&fixture, "a bold word");
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 2));
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 5));
    /* The space that followed the run was not bold and must not have become so. */
    g_assert_false(has_tag_at(&fixture, TAG_STRONG, 6));

    undo_record_free(record);
    fixture_clear(&fixture);
}

/* Text put back inside a styled run must wear what the record says, not what
 * the place it lands in says. GTK gives inserted text the tags covering the
 * spot, so without clearing them first this comes back bold. */
static void test_text_put_back_does_not_inherit_its_surroundings(void)
{
    Fixture fixture;
    fixture_init(&fixture, "bold");
    apply_tag(&fixture, TAG_STRONG, 0, 4);

    /* A plain record, dropped into the middle of a bold word. */
    UndoRecord* record = text_record(UNDO_TEXT_DELETE, 2, "XY");

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, TRUE);
    assert_text(&fixture, "boXYld");
    g_assert_false(has_tag_at(&fixture, TAG_STRONG, 2));
    g_assert_false(has_tag_at(&fixture, TAG_STRONG, 3));
    /* And the bold either side of it is untouched. */
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 1));
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 4));

    undo_record_free(record);
    fixture_clear(&fixture);
}

/* The case a second press could not undo. Bold over a half-bold selection makes
 * all of it bold; pressing bold again would then clear the lot, including the
 * part that was bold to start with. Only the record still knows the mix. */
static void test_bold_over_a_mixed_selection_restores_the_mix(void)
{
    Fixture fixture;
    fixture_init(&fixture, "bold plain");
    apply_tag(&fixture, TAG_STRONG, 0, 4);

    UndoRecord* record = undo_record_capture_style(fixture.buffer,
                                                   fixture.tags[TAG_STRONG],
                                                   TAG_STRONG, 0, 10, TRUE);
    g_assert_nonnull(record);

    /* The press: the whole selection becomes bold. */
    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, FALSE);
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 7));

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, TRUE);
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 0));
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 3));
    g_assert_false(has_tag_at(&fixture, TAG_STRONG, 4));
    g_assert_false(has_tag_at(&fixture, TAG_STRONG, 7));

    undo_record_free(record);
    fixture_clear(&fixture);
}

/* Turning a style off and taking that back is the same rule read the other
 * way. */
static void test_taking_bold_off_can_be_undone(void)
{
    Fixture fixture;
    fixture_init(&fixture, "bold");
    apply_tag(&fixture, TAG_STRONG, 0, 4);

    UndoRecord* record = undo_record_capture_style(
        fixture.buffer, fixture.tags[TAG_STRONG], TAG_STRONG, 0, 4, FALSE);

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, FALSE);
    g_assert_false(has_tag_at(&fixture, TAG_STRONG, 0));

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, TRUE);
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 0));
    g_assert_true(has_tag_at(&fixture, TAG_STRONG, 3));

    undo_record_free(record);
    fixture_clear(&fixture);
}

/* Offsets are characters, not bytes. Anything using strlen() to work out where
 * text ends puts it back in the wrong place the first time an author types an
 * accent. */
static void test_offsets_count_characters_not_bytes(void)
{
    Fixture fixture;
    fixture_init(&fixture, "café bar");
    apply_tag(&fixture, TAG_EMPHASIS, 5, 8);

    UndoRecord* record = undo_record_capture_text(
        fixture.buffer, fixture.tags, TAG_COUNT, UNDO_TEXT_INSERT, 5, 8);

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, TRUE);
    assert_text(&fixture, "café ");

    undo_record_apply(record, fixture.buffer, fixture.tags, TAG_COUNT, FALSE);
    assert_text(&fixture, "café bar");
    g_assert_true(has_tag_at(&fixture, TAG_EMPHASIS, 5));
    g_assert_false(has_tag_at(&fixture, TAG_EMPHASIS, 0));

    undo_record_free(record);
    fixture_clear(&fixture);
}

/* ── the store ───────────────────────────────────────────────────────────── */

#define DOC "/project/manuscript/scene.md"
#define OTHER "/project/manuscript/notes.md"

static void test_a_run_of_typing_is_one_press(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "h"));
    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 1, "i"));
    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 2, " "));
    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 3, "t"));

    const UndoRecord* top = undo_store_peek_undo(store, DOC);
    g_assert_nonnull(top);
    g_assert_cmpstr(top->text.text, ==, "t");

    undo_store_step_undo(store, DOC);
    top = undo_store_peek_undo(store, DOC);
    g_assert_nonnull(top);
    g_assert_cmpstr(top->text.text, ==, "hi ");
    g_assert_cmpint(top->text.from, ==, 0);

    undo_store_step_undo(store, DOC);
    g_assert_null(undo_store_peek_undo(store, DOC));

    undo_store_free(store);
}

/* Backspacing builds the run leftwards, so what comes back is the word the
 * right way round. */
static void test_backspacing_rebuilds_the_word(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_DELETE, 4, "d"));
    undo_store_push(store, DOC, text_record(UNDO_TEXT_DELETE, 3, "l"));
    undo_store_push(store, DOC, text_record(UNDO_TEXT_DELETE, 2, "o"));

    const UndoRecord* top = undo_store_peek_undo(store, DOC);
    g_assert_nonnull(top);
    g_assert_cmpstr(top->text.text, ==, "old");
    g_assert_cmpint(top->text.from, ==, 2);

    undo_store_free(store);
}

/* The cursor moving ends the run: two stretches of typing with a click between
 * them are two things done, however close together the offsets fall. */
static void test_moving_the_cursor_ends_the_run(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "hi"));
    undo_store_break_run(store, DOC);
    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 2, "there"));

    const UndoRecord* top = undo_store_peek_undo(store, DOC);
    g_assert_cmpstr(top->text.text, ==, "there");

    undo_store_step_undo(store, DOC);
    g_assert_cmpstr(undo_store_peek_undo(store, DOC)->text.text, ==, "hi");

    undo_store_free(store);
}

/* A new action is a new future: whatever had been undone and not redone is no
 * longer reachable. */
static void test_a_new_action_drops_the_redo_tail(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "one"));
    undo_store_break_run(store, DOC);
    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 3, "two"));

    undo_store_step_undo(store, DOC);
    g_assert_nonnull(undo_store_peek_redo(store, DOC));

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 3, "three"));
    g_assert_null(undo_store_peek_redo(store, DOC));
    g_assert_cmpstr(undo_store_peek_undo(store, DOC)->text.text, ==, "three");

    undo_store_free(store);
}

/* Redo hands the record back and undo takes it again, without either losing it
 * on the way. */
static void test_undo_and_redo_walk_the_same_records(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "one"));
    undo_store_break_run(store, DOC);
    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 3, "two"));

    undo_store_step_undo(store, DOC);
    undo_store_step_undo(store, DOC);
    g_assert_null(undo_store_peek_undo(store, DOC));
    g_assert_cmpstr(undo_store_peek_redo(store, DOC)->text.text, ==, "one");

    undo_store_step_redo(store, DOC);
    g_assert_cmpstr(undo_store_peek_undo(store, DOC)->text.text, ==, "one");
    g_assert_cmpstr(undo_store_peek_redo(store, DOC)->text.text, ==, "two");

    /* And stepping past the end changes nothing. */
    undo_store_step_redo(store, DOC);
    undo_store_step_redo(store, DOC);
    g_assert_null(undo_store_peek_redo(store, DOC));

    undo_store_free(store);
}

/* One history per item. Typing in one document must not turn up in another's
 * history, which is what keeps Ctrl+Z from leaving the document on screen. */
static void test_each_item_keeps_its_own_history(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "scene"));
    undo_store_push(store, OTHER, text_record(UNDO_TEXT_INSERT, 0, "notes"));

    g_assert_cmpstr(undo_store_peek_undo(store, DOC)->text.text, ==, "scene");
    g_assert_cmpstr(undo_store_peek_undo(store, OTHER)->text.text, ==, "notes");
    g_assert_null(undo_store_peek_undo(store, "/project/manuscript/nothing.md"));

    /* Forgetting one leaves the other alone. */
    undo_store_forget(store, DOC);
    g_assert_null(undo_store_peek_undo(store, DOC));
    g_assert_nonnull(undo_store_peek_undo(store, OTHER));

    undo_store_free(store);
}

/* A history whose offsets no longer describe the text is worse than no history:
 * replaying it would put characters back in the wrong places. Save normalises
 * markup, so a document coming back changed is a real outcome. */
static void test_a_changed_document_loses_its_history(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "hello"));
    undo_store_note_fingerprint(store, DOC, 12345);

    /* Back with the text it was left with: the history stands. */
    undo_store_check_fingerprint(store, DOC, 12345);
    g_assert_nonnull(undo_store_peek_undo(store, DOC));

    /* Back with something else: it goes. */
    undo_store_check_fingerprint(store, DOC, 99999);
    g_assert_null(undo_store_peek_undo(store, DOC));

    undo_store_free(store);
}

/* A history that was never noted as left cannot be vouched for either, so it
 * goes the same way rather than being replayed on trust. */
static void test_a_history_never_noted_is_not_trusted(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "hello"));
    undo_store_check_fingerprint(store, DOC, 12345);
    g_assert_null(undo_store_peek_undo(store, DOC));

    /* And a path with no history at all is not a failure. */
    undo_store_check_fingerprint(store, OTHER, 1);

    undo_store_free(store);
}

/* The fingerprint has to answer for the text, or the check above lets a changed
 * document through. */
static void test_the_fingerprint_follows_the_text(void)
{
    Fixture fixture;
    fixture_init(&fixture, "hello");
    const guint64 first = undo_fingerprint(fixture.buffer);

    gtk_text_buffer_set_text(fixture.buffer, "hello", -1);
    g_assert_cmpuint(undo_fingerprint(fixture.buffer), ==, first);

    gtk_text_buffer_set_text(fixture.buffer, "hello ", -1);
    g_assert_cmpuint(undo_fingerprint(fixture.buffer), !=, first);

    fixture_clear(&fixture);
}

/* Closing the project is the whole of "session-scoped". */
static void test_clearing_forgets_everything(void)
{
    UndoStore* store = undo_store_new();

    undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, 0, "one"));
    undo_store_push(store, OTHER, text_record(UNDO_TEXT_INSERT, 0, "two"));

    undo_store_clear(store);
    g_assert_null(undo_store_peek_undo(store, DOC));
    g_assert_null(undo_store_peek_undo(store, OTHER));

    undo_store_free(store);
}

/* A day of typing must not grow without a bound, and the cursor has to follow
 * the records it still describes when the oldest are dropped. */
static void test_a_history_is_bounded(void)
{
    UndoStore* store = undo_store_new();

    for (int index = 0; index < UNDO_HISTORY_LIMIT + 50; index++) {
        undo_store_push(store, DOC, text_record(UNDO_TEXT_INSERT, index, "x"));
        undo_store_break_run(store, DOC);
    }

    /* Everything is still reachable backwards, and the count has stopped
     * growing: stepping back the whole limit runs out exactly there. */
    for (int index = 0; index < UNDO_HISTORY_LIMIT; index++) {
        g_assert_nonnull(undo_store_peek_undo(store, DOC));
        undo_store_step_undo(store, DOC);
    }
    g_assert_null(undo_store_peek_undo(store, DOC));

    undo_store_free(store);
}

/* ── what the menu says ──────────────────────────────────────────────────── */

/* The Edit menu names what a press would take back, so a metadata record has to
 * answer with its own field rather than a fixed word. */
static void test_a_record_names_itself(void)
{
    UndoRecord* typing = text_record(UNDO_TEXT_INSERT, 0, "x");
    char*       verb   = undo_record_verb(typing);
    g_assert_cmpstr(verb, ==, "Typing");
    g_free(verb);
    undo_record_free(typing);

    UndoRecord* bold = g_new0(UndoRecord, 1);
    bold->kind            = UNDO_STYLE;
    bold->style.tag_index = TAG_STRONG;
    verb                  = undo_record_verb(bold);
    g_assert_cmpstr(verb, ==, "Bold");
    g_free(verb);
    undo_record_free(bold);

    UndoRecord* field =
        undo_record_new_metadata("/project/manuscript/scene.md", FALSE, "synopsis",
                                 NULL, NULL, "a first draft", NULL);
    verb = undo_record_verb(field);
    g_assert_cmpstr(verb, ==, "Synopsis");
    g_free(verb);
    undo_record_free(field);

    /* Nothing to take back is not a verb. */
    g_assert_null(undo_record_verb(NULL));
}

/* One keystroke has to cost one press to take back, so the several records a
 * keystroke can make are gathered into one. Enter inside a list is the case
 * that forced it: a newline and the item it opens below. */
static void test_a_compound_is_one_thing_done(void)
{
    UndoRecord* first  = text_record(UNDO_TEXT_INSERT, 0, "\n");
    UndoBlockLine line = { 1, EDITOR_BLOCK_PARAGRAPH, EDITOR_BLOCK_BULLET_LIST };
    UndoRecord*   second = undo_record_new_block("Bulleted List", &line, 1);

    UndoRecord* parts[2] = { first, second };
    UndoRecord* compound = undo_record_new_compound(parts, 2);

    g_assert_nonnull(compound);
    g_assert_cmpint(compound->kind, ==, UNDO_COMPOUND);
    g_assert_cmpuint(compound->compound.part_count, ==, 2);
    /* In the order they were handed over: that is what applying them backwards
     * in reverse depends on. */
    g_assert_true(compound->compound.parts[0] == first);
    g_assert_true(compound->compound.parts[1] == second);

    /* Named for the gesture, which is the part that came first. */
    char* verb = undo_record_verb(compound);
    g_assert_cmpstr(verb, ==, "Typing");
    g_free(verb);

    undo_record_free(compound);   /* and both parts with it */
}

/* A caller that may or may not have a second thing to add should not have to
 * branch, so the NULLs are dropped here and one survivor is not wrapped. */
static void test_a_compound_of_one_thing_is_that_thing(void)
{
    UndoRecord* only     = text_record(UNDO_TEXT_INSERT, 0, "x");
    UndoRecord* parts[2] = { only, NULL };

    UndoRecord* made = undo_record_new_compound(parts, 2);
    g_assert_true(made == only);
    g_assert_cmpint(made->kind, ==, UNDO_TEXT_INSERT);
    undo_record_free(made);

    /* A NULL first and a record second is the same answer, not a hole. */
    UndoRecord* second     = text_record(UNDO_TEXT_INSERT, 0, "y");
    UndoRecord* sparse[2]  = { NULL, second };
    g_assert_true(undo_record_new_compound(sparse, 2) == second);
    undo_record_free(second);

    /* And nothing at all is nothing, which record_edit() already knows to
     * throw away. */
    UndoRecord* empty[2] = { NULL, NULL };
    g_assert_null(undo_record_new_compound(empty, 2));
    g_assert_null(undo_record_new_compound(NULL, 0));
}

/* A metadata record has to hold absent, a scalar and a sequence apart: emptying
 * a field removes it, and undo has to be able to put that difference back. */
static void test_a_metadata_record_keeps_all_three_kinds_of_value(void)
{
    const char* const keywords[] = { "ghosts", "rain", NULL };

    UndoRecord* record =
        undo_record_new_metadata("/project/manuscript/scene.md", FALSE, "keywords",
                                 NULL, NULL, NULL, keywords);
    g_assert_nonnull(record);

    /* Before: absent, which is neither a scalar nor an empty list. */
    g_assert_null(record->metadata.before.scalar);
    g_assert_null(record->metadata.before.items);

    g_assert_null(record->metadata.after.scalar);
    g_assert_nonnull(record->metadata.after.items);
    g_assert_cmpstr(record->metadata.after.items[0], ==, "ghosts");
    g_assert_cmpstr(record->metadata.after.items[1], ==, "rain");
    g_assert_null(record->metadata.after.items[2]);

    /* And a metadata record is never coalesced into anything. */
    UndoRecord* second =
        undo_record_new_metadata("/project/manuscript/scene.md", FALSE, "keywords",
                                 NULL, keywords, "one", NULL);
    g_assert_false(undo_records_coalesce(record, second));

    undo_record_free(record);
    undo_record_free(second);
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/undo/typing-a-word-is-one-entry",
                    test_typing_a_word_is_one_entry);
    g_test_add_func("/undo/run-breaks-at-the-next-word",
                    test_a_run_breaks_at_the_next_word);
    g_test_add_func("/undo/newline-ends-a-run", test_a_newline_ends_a_run);
    g_test_add_func("/undo/deletions-coalesce-both-ways",
                    test_deletions_coalesce_in_both_directions);

    g_test_add_func("/undo/redo-restores-styling",
                    test_redoing_an_insertion_restores_its_styling);
    g_test_add_func("/undo/deleted-bold-comes-back-bold",
                    test_undoing_a_deletion_restores_bold_as_bold);
    g_test_add_func("/undo/text-does-not-inherit-its-surroundings",
                    test_text_put_back_does_not_inherit_its_surroundings);
    g_test_add_func("/undo/mixed-selection-restores-the-mix",
                    test_bold_over_a_mixed_selection_restores_the_mix);
    g_test_add_func("/undo/bold-off-can-be-undone",
                    test_taking_bold_off_can_be_undone);
    g_test_add_func("/undo/offsets-count-characters",
                    test_offsets_count_characters_not_bytes);

    g_test_add_func("/undo/store/run-of-typing-is-one-press",
                    test_a_run_of_typing_is_one_press);
    g_test_add_func("/undo/store/backspacing-rebuilds-the-word",
                    test_backspacing_rebuilds_the_word);
    g_test_add_func("/undo/store/cursor-move-ends-the-run",
                    test_moving_the_cursor_ends_the_run);
    g_test_add_func("/undo/store/new-action-drops-the-tail",
                    test_a_new_action_drops_the_redo_tail);
    g_test_add_func("/undo/store/undo-and-redo-walk-together",
                    test_undo_and_redo_walk_the_same_records);
    g_test_add_func("/undo/store/one-history-per-item",
                    test_each_item_keeps_its_own_history);
    g_test_add_func("/undo/store/changed-document-loses-history",
                    test_a_changed_document_loses_its_history);
    g_test_add_func("/undo/store/unvouched-history-is-dropped",
                    test_a_history_never_noted_is_not_trusted);
    g_test_add_func("/undo/store/fingerprint-follows-the-text",
                    test_the_fingerprint_follows_the_text);
    g_test_add_func("/undo/store/clearing-forgets-everything",
                    test_clearing_forgets_everything);
    g_test_add_func("/undo/store/history-is-bounded", test_a_history_is_bounded);

    g_test_add_func("/undo/compound/is-one-thing-done",
                    test_a_compound_is_one_thing_done);
    g_test_add_func("/undo/compound/of-one-is-that-thing",
                    test_a_compound_of_one_thing_is_that_thing);

    g_test_add_func("/undo/menu/record-names-itself", test_a_record_names_itself);
    g_test_add_func("/undo/menu/metadata-keeps-three-kinds",
                    test_a_metadata_record_keeps_all_three_kinds_of_value);

    return g_test_run();
}
