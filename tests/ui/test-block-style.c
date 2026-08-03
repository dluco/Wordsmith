#include "ui/editor-panel.h"

#include <gtk/gtk.h>

/* What the format bar's dropdown shows, and what picking from it does, are
 * editor_block_style_at() and editor_block_apply() — so this is where both are
 * tested. A GtkTextBuffer and its tags are plain objects, the same reason
 * test-format-bar.c can check the inline half without a display.
 *
 * The fixture builds the tags the editor panel builds, under the names the
 * panel gives them. What matters to these rules is not what a heading looks
 * like but which lines carry which tag, so the properties are left off. */

typedef struct Fixture {
    GtkTextBuffer*  buffer;
    EditorBlockTags tags;
} Fixture;

static GtkTextTag* make_tag(Fixture* fixture, const char* name)
{
    return gtk_text_buffer_create_tag(fixture->buffer, name, NULL);
}

static void fixture_init(Fixture* fixture, const char* text)
{
    fixture->buffer = gtk_text_buffer_new(NULL);
    memset(&fixture->tags, 0, sizeof(fixture->tags));

    fixture->tags.styles[EDITOR_BLOCK_HEADING_1]     = make_tag(fixture, "heading-1");
    fixture->tags.styles[EDITOR_BLOCK_HEADING_2]     = make_tag(fixture, "heading-2");
    fixture->tags.styles[EDITOR_BLOCK_HEADING_3]     = make_tag(fixture, "heading-3");
    fixture->tags.styles[EDITOR_BLOCK_QUOTE]         = make_tag(fixture, "quote");
    fixture->tags.styles[EDITOR_BLOCK_BULLET_LIST]   = make_tag(fixture, "list-bullet");
    fixture->tags.styles[EDITOR_BLOCK_NUMBERED_LIST] = make_tag(fixture, "list-ordered");

    fixture->tags.unoffered[0] = make_tag(fixture, "code-block");
    fixture->tags.unoffered[1] = make_tag(fixture, "heading-4");

    fixture->tags.marker = make_tag(fixture, "list-marker");

    gtk_text_buffer_set_text(fixture->buffer, text, -1);
}

static void fixture_clear(Fixture* fixture)
{
    g_object_unref(fixture->buffer);
}

static EditorBlockStyle style_at(Fixture* fixture, int line)
{
    return editor_block_style_at(fixture->buffer, &fixture->tags, line);
}

static void set_style(Fixture* fixture, int first, int last, EditorBlockStyle style)
{
    editor_block_apply(fixture->buffer, &fixture->tags, first, last, style);
    editor_block_renumber(fixture->buffer, &fixture->tags, first - 1, last + 1);
}

/* Apply a tag by hand across a line and its newline, which is the coverage
 * editor_block_apply() produces and the coverage the reader expects. */
static void tag_line(Fixture* fixture, GtkTextTag* tag, int line)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_line(fixture->buffer, &start, line);
    end = start;
    if (!gtk_text_iter_forward_line(&end)) {
        gtk_text_buffer_get_end_iter(fixture->buffer, &end);
    }
    gtk_text_buffer_apply_tag(fixture->buffer, tag, &start, &end);
}

static char* line_text(Fixture* fixture, int line)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_line(fixture->buffer, &start, line);
    end = start;
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }
    return gtk_text_buffer_get_text(fixture->buffer, &start, &end, FALSE);
}

static void assert_line(Fixture* fixture, int line, const char* expected)
{
    char* actual = line_text(fixture, line);
    g_assert_cmpstr(actual, ==, expected);
    g_free(actual);
}

/* A paragraph is the absence of every other tag, not a tag of its own, so an
 * untouched buffer reads as paragraphs all the way down. */
static void test_untagged_lines_are_paragraphs(void)
{
    Fixture fixture;
    fixture_init(&fixture, "one\ntwo\nthree");

    g_assert_cmpint(style_at(&fixture, 0), ==, EDITOR_BLOCK_PARAGRAPH);
    g_assert_cmpint(style_at(&fixture, 2), ==, EDITOR_BLOCK_PARAGRAPH);

    /* Off the end is not a paragraph — there is no line there to be one. */
    g_assert_cmpint(style_at(&fixture, 3), ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(style_at(&fixture, -1), ==, EDITOR_BLOCK_OTHER);

    fixture_clear(&fixture);
}

static void test_a_pick_replaces_what_was_there(void)
{
    Fixture fixture;
    fixture_init(&fixture, "title\nbody\nmore");

    set_style(&fixture, 0, 0, EDITOR_BLOCK_HEADING_1);
    g_assert_cmpint(style_at(&fixture, 0), ==, EDITOR_BLOCK_HEADING_1);
    g_assert_cmpint(style_at(&fixture, 1), ==, EDITOR_BLOCK_PARAGRAPH);

    /* The kinds are exclusive: asking for a second one is not adding it. */
    set_style(&fixture, 0, 0, EDITOR_BLOCK_QUOTE);
    g_assert_cmpint(style_at(&fixture, 0), ==, EDITOR_BLOCK_QUOTE);

    /* And Paragraph is the way back, which is why it is on the list. */
    set_style(&fixture, 0, 0, EDITOR_BLOCK_PARAGRAPH);
    g_assert_cmpint(style_at(&fixture, 0), ==, EDITOR_BLOCK_PARAGRAPH);

    fixture_clear(&fixture);
}

/* The text of the line is untouched by any of it — only a list marker ever
 * moves a character, and a heading moves none. */
static void test_a_pick_leaves_the_words_alone(void)
{
    Fixture fixture;
    fixture_init(&fixture, "the words\nsecond");

    set_style(&fixture, 0, 0, EDITOR_BLOCK_HEADING_2);
    assert_line(&fixture, 0, "the words");

    set_style(&fixture, 0, 0, EDITOR_BLOCK_PARAGRAPH);
    assert_line(&fixture, 0, "the words");
    g_assert_cmpint(gtk_text_buffer_get_line_count(fixture.buffer), ==, 2);

    fixture_clear(&fixture);
}

/* An empty line has one character — its newline — and the tag has to live
 * there, or "make this line a heading and start typing" would have nowhere to
 * put the answer between the pick and the first keystroke. */
static void test_an_empty_line_can_carry_a_style(void)
{
    Fixture fixture;
    fixture_init(&fixture, "before\n\nafter");

    set_style(&fixture, 1, 1, EDITOR_BLOCK_HEADING_1);
    g_assert_cmpint(style_at(&fixture, 1), ==, EDITOR_BLOCK_HEADING_1);

    /* And it stays this line's answer rather than leaking onto its neighbours,
     * which covering the newline is the easiest way to get wrong. */
    g_assert_cmpint(style_at(&fixture, 0), ==, EDITOR_BLOCK_PARAGRAPH);
    g_assert_cmpint(style_at(&fixture, 2), ==, EDITOR_BLOCK_PARAGRAPH);

    fixture_clear(&fixture);
}

/* The last line of a buffer has no newline at all, so the range a tag goes on
 * stops at the end of the text rather than one past it. */
static void test_the_last_line_can_carry_a_style(void)
{
    Fixture fixture;
    fixture_init(&fixture, "first\nlast");

    set_style(&fixture, 1, 1, EDITOR_BLOCK_QUOTE);
    g_assert_cmpint(style_at(&fixture, 1), ==, EDITOR_BLOCK_QUOTE);
    assert_line(&fixture, 1, "last");

    fixture_clear(&fixture);
}

static void test_a_bulleted_list_gains_and_loses_its_markers(void)
{
    Fixture fixture;
    fixture_init(&fixture, "milk\neggs\nbread");

    set_style(&fixture, 0, 2, EDITOR_BLOCK_BULLET_LIST);
    assert_line(&fixture, 0, "- milk");
    assert_line(&fixture, 1, "- eggs");
    assert_line(&fixture, 2, "- bread");

    /* Going back takes the scaffolding with it: a marker is display, not text,
     * and leaving one behind would put it in the author's manuscript. */
    set_style(&fixture, 0, 2, EDITOR_BLOCK_PARAGRAPH);
    assert_line(&fixture, 0, "milk");
    assert_line(&fixture, 1, "eggs");
    assert_line(&fixture, 2, "bread");

    fixture_clear(&fixture);
}

/* A numbered list counts, and a second pick over the same lines does not stack
 * a marker on top of the one already there. */
static void test_a_numbered_list_counts(void)
{
    Fixture fixture;
    fixture_init(&fixture, "one\ntwo\nthree");

    set_style(&fixture, 0, 2, EDITOR_BLOCK_NUMBERED_LIST);
    assert_line(&fixture, 0, "1. one");
    assert_line(&fixture, 1, "2. two");
    assert_line(&fixture, 2, "3. three");

    set_style(&fixture, 0, 2, EDITOR_BLOCK_BULLET_LIST);
    assert_line(&fixture, 0, "- one");
    assert_line(&fixture, 2, "- three");

    fixture_clear(&fixture);
}

/* A run that is broken counts from 1 again on the far side, and a run the
 * caller only clipped is still renumbered to its own ends — inserting an item
 * into the middle of ten must not restart the count there. */
static void test_a_numbered_run_is_renumbered_whole(void)
{
    Fixture fixture;
    fixture_init(&fixture, "a\nb\nc\nd\ne");

    set_style(&fixture, 0, 4, EDITOR_BLOCK_NUMBERED_LIST);
    assert_line(&fixture, 4, "5. e");

    /* Break the middle out of the run. What is left is two runs, each counting
     * from 1, and only the second one moved. */
    set_style(&fixture, 2, 2, EDITOR_BLOCK_PARAGRAPH);
    assert_line(&fixture, 0, "1. a");
    assert_line(&fixture, 1, "2. b");
    assert_line(&fixture, 2, "c");
    assert_line(&fixture, 3, "1. d");
    assert_line(&fixture, 4, "2. e");

    /* Join them back up by naming only the line in the gap. The runs either
     * side are reached out to, so the count runs 1 to 5 and not 1, 2, 1, 1, 2. */
    set_style(&fixture, 2, 2, EDITOR_BLOCK_NUMBERED_LIST);
    assert_line(&fixture, 3, "4. d");
    assert_line(&fixture, 4, "5. e");

    fixture_clear(&fixture);
}

/* A line the UI has no name for answers EDITOR_BLOCK_OTHER rather than passing
 * for a paragraph, which is what stops the dropdown from showing an answer it
 * could not give back. */
static void test_an_unoffered_kind_is_not_a_paragraph(void)
{
    Fixture fixture;
    fixture_init(&fixture, "code\nprose\ndeep heading");

    tag_line(&fixture, fixture.tags.unoffered[0], 0);   /* a code block */
    tag_line(&fixture, fixture.tags.unoffered[1], 2);   /* a heading 4 */

    g_assert_cmpint(style_at(&fixture, 0), ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(style_at(&fixture, 1), ==, EDITOR_BLOCK_PARAGRAPH);
    g_assert_cmpint(style_at(&fixture, 2), ==, EDITOR_BLOCK_OTHER);

    fixture_clear(&fixture);
}

/* The two list styles have an off and the other five do not, because the lists
 * are the ones with a button and a lit button means "click to take this off". */
static void test_only_the_lists_have_an_off(void)
{
    /* A list asked for where that list already covers everything addressed is
     * that list being taken off. */
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_BULLET_LIST,
                                                 EDITOR_BLOCK_BULLET_LIST),
                    ==, EDITOR_BLOCK_PARAGRAPH);
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_NUMBERED_LIST,
                                                 EDITOR_BLOCK_NUMBERED_LIST),
                    ==, EDITOR_BLOCK_PARAGRAPH);

    /* The two lists are different answers, not one: asking for numbers over
     * bullets swaps them rather than turning the list off. */
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_NUMBERED_LIST,
                                                 EDITOR_BLOCK_BULLET_LIST),
                    ==, EDITOR_BLOCK_NUMBERED_LIST);

    /* Not covering the whole of what is addressed is not in force. A selection
     * half in a list becomes a list, the way a half-bold one becomes bold. */
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_BULLET_LIST,
                                                 EDITOR_BLOCK_OTHER),
                    ==, EDITOR_BLOCK_BULLET_LIST);
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_BULLET_LIST,
                                                 EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_BULLET_LIST);

    /* The five reached from the dropdown are a straight pick either way:
     * asking for the kind a line already has is not a way back to a paragraph,
     * because picking the same row twice is not a gesture that means off. */
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_HEADING_1,
                                                 EDITOR_BLOCK_HEADING_1),
                    ==, EDITOR_BLOCK_HEADING_1);
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_QUOTE,
                                                 EDITOR_BLOCK_QUOTE),
                    ==, EDITOR_BLOCK_QUOTE);
    g_assert_cmpint(editor_block_style_for_press(EDITOR_BLOCK_PARAGRAPH,
                                                 EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_PARAGRAPH);
}

/* Typing `- ` or `1. ` at the head of a paragraph asks for a list; nothing else
 * asks for anything. The rule is deliberately narrow, because every case it
 * gets wrong is a character an author typed being taken away from them. */
static void test_a_typed_marker_asks_for_a_list(void)
{
    g_assert_cmpint(editor_autoformat_style(" ", 1, "- ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_BULLET_LIST);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "1. ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_NUMBERED_LIST);

    /* Only a paragraph. A hyphen at the head of a heading is a hyphen, and one
     * at the head of an item is the author writing a dash into their list. */
    g_assert_cmpint(editor_autoformat_style(" ", 1, "- ", EDITOR_BLOCK_HEADING_1),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "- ", EDITOR_BLOCK_QUOTE),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "- ", EDITOR_BLOCK_BULLET_LIST),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "- ", EDITOR_BLOCK_OTHER),
                    ==, EDITOR_BLOCK_OTHER);

    /* Only the space. Typing the marker is not asking for anything yet — a
     * hyphen on its own is a line an author may be about to finish. */
    g_assert_cmpint(editor_autoformat_style("-", 1, "-", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(".", 1, "1.", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);

    /* And only a lone space, the rule the newline follows: a paste that happens
     * to carry one is not a key being pressed. */
    g_assert_cmpint(editor_autoformat_style(" ", 2, "- ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style("- ", 2, "- ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(NULL, 1, "- ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, NULL, EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);

    /* Nothing else on the line, which is also how the cursor is known to be at
     * the end of it and the marker at the start. A space typed into `-word` or
     * in front of a hyphen leaves both alone. */
    g_assert_cmpint(editor_autoformat_style(" ", 1, "- word", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, " -", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "word - ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);

    /* `1.` and no other number: the number is not kept, so `3. ` would answer a
     * request to start at three by starting at one. Nor the other markers
     * Markdown allows, which few authors type and none would miss. */
    g_assert_cmpint(editor_autoformat_style(" ", 1, "3. ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "1) ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "* ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_autoformat_style(" ", 1, "+ ", EDITOR_BLOCK_PARAGRAPH),
                    ==, EDITOR_BLOCK_OTHER);
}

/* What a conversion has to be able to put back. The marker the author typed and
 * the marker the list draws are the same three characters, so undoing the pick
 * has to take the drawn one off before the typed one goes back — this is the
 * half of that a display-free test can hold: applying the style regenerates the
 * marker, and taking the style off regenerates its absence. */
static void test_a_converted_line_gives_its_marker_back(void)
{
    Fixture fixture;
    fixture_init(&fixture, "first\n\nthird");

    /* The line as the conversion leaves it: emptied of what was typed, then
     * made a list. */
    set_style(&fixture, 1, 1, EDITOR_BLOCK_BULLET_LIST);
    g_assert_cmpint(style_at(&fixture, 1), ==, EDITOR_BLOCK_BULLET_LIST);
    assert_line(&fixture, 1, "- ");

    /* And as undoing it leaves the line: a paragraph with nothing on it, ready
     * for the typed characters to go back. */
    set_style(&fixture, 1, 1, EDITOR_BLOCK_PARAGRAPH);
    g_assert_cmpint(style_at(&fixture, 1), ==, EDITOR_BLOCK_PARAGRAPH);
    assert_line(&fixture, 1, "");

    fixture_clear(&fixture);
}

/* A return carries a list on, and a return on an item with nothing in it lets
 * the list go. Nothing else about Enter changed. */
static void test_a_return_carries_a_list_on(void)
{
    /* In an item with words in it: another item below. */
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_BULLET_LIST, FALSE),
                    ==, EDITOR_RETURN_CONTINUE_LIST);
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_NUMBERED_LIST, FALSE),
                    ==, EDITOR_RETURN_CONTINUE_LIST);

    /* In an item holding nothing but its marker: the list ends, and the newline
     * has to be refused rather than followed by a correction. */
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_BULLET_LIST, TRUE),
                    ==, EDITOR_RETURN_END_LIST);
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_NUMBERED_LIST, TRUE),
                    ==, EDITOR_RETURN_END_LIST);

    /* Every other kind opens a plain line, empty or not. A title is one line,
     * and the thing after it is not another title. */
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_HEADING_1, FALSE),
                    ==, EDITOR_RETURN_ORDINARY);
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_QUOTE, FALSE),
                    ==, EDITOR_RETURN_ORDINARY);
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_PARAGRAPH, TRUE),
                    ==, EDITOR_RETURN_ORDINARY);
    g_assert_cmpint(editor_return_action("\n", 1, EDITOR_BLOCK_OTHER, FALSE),
                    ==, EDITOR_RETURN_ORDINARY);

    /* Only a lone newline is a return being pressed. A paste carrying one is
     * not, and must not turn the page it brought into a list. */
    g_assert_cmpint(editor_return_action("\nsecond", 7, EDITOR_BLOCK_BULLET_LIST,
                                         FALSE),
                    ==, EDITOR_RETURN_ORDINARY);
    g_assert_cmpint(editor_return_action("first\n", 6, EDITOR_BLOCK_BULLET_LIST,
                                         FALSE),
                    ==, EDITOR_RETURN_ORDINARY);

    /* And ordinary typing in a list is ordinary typing. */
    g_assert_cmpint(editor_return_action("a", 1, EDITOR_BLOCK_BULLET_LIST, FALSE),
                    ==, EDITOR_RETURN_ORDINARY);
    g_assert_cmpint(editor_return_action(NULL, 1, EDITOR_BLOCK_BULLET_LIST, FALSE),
                    ==, EDITOR_RETURN_ORDINARY);
}

/* Backspace is how an author leaves a list, and a marker is non-editable — so
 * the press has to be read as a rule rather than left to the buffer, which
 * refuses it and leaves them stuck behind a `- ` no key removes. */
static void test_a_backspace_leaves_a_list(void)
{
    const gboolean back = FALSE;

    /* Just past the marker, which is where deleting the last word of an item
     * leaves the caret. This is the press that did nothing at all. */
    g_assert_true(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_BULLET_LIST, 2, 2));
    g_assert_true(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_NUMBERED_LIST, 3, 3));

    /* Inside it, which arrow keys can reach: the press is refused there too. */
    g_assert_true(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_BULLET_LIST, 2, 1));
    g_assert_true(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_NUMBERED_LIST, 3, 2));

    /* And at the head of the line, where the press is not refused but is worse:
     * it would carry the marker into the middle of the line above, where
     * nothing draws it and save drops it. */
    g_assert_true(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_BULLET_LIST, 2, 0));

    /* Past the marker is the author's own text, and a backspace there deletes a
     * character like anywhere else. */
    g_assert_false(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_BULLET_LIST, 2, 3));
    g_assert_false(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_BULLET_LIST, 2, 12));

    /* A backspace over a selection deletes the selection, wherever it starts. */
    g_assert_false(
        editor_delete_leaves_list(TRUE, back, EDITOR_BLOCK_BULLET_LIST, 2, 2));
    g_assert_false(
        editor_delete_leaves_list(TRUE, back, EDITOR_BLOCK_BULLET_LIST, 2, 0));

    /* Every other kind draws no marker, so nothing stands in the way of an
     * ordinary deletion. */
    g_assert_false(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_PARAGRAPH, 0, 0));
    g_assert_false(editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_QUOTE, 0, 0));
    g_assert_false(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_HEADING_1, 0, 0));
    g_assert_false(editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_OTHER, 0, 0));

    /* A list with no marker to run into is not this rule's business either —
     * there is nothing there a deletion could be refused by. */
    g_assert_false(
        editor_delete_leaves_list(FALSE, back, EDITOR_BLOCK_BULLET_LIST, 0, 0));
}

/* Delete is the same rule from the other side, and it reaches one line further:
 * a forward press at the end of a line eats the newline, so the marker below is
 * what it runs into. */
static void test_a_delete_leaves_a_list(void)
{
    const gboolean forward = TRUE;

    /* At the head of an item, and inside the marker: the marker is what lies
     * ahead, so the press is refused and does nothing. */
    g_assert_true(
        editor_delete_leaves_list(FALSE, forward, EDITOR_BLOCK_BULLET_LIST, 2, 0));
    g_assert_true(
        editor_delete_leaves_list(FALSE, forward, EDITOR_BLOCK_BULLET_LIST, 2, 1));
    g_assert_true(
        editor_delete_leaves_list(FALSE, forward, EDITOR_BLOCK_NUMBERED_LIST, 3, 2));

    /* The far side of the marker is where the two directions differ, and each
     * takes the column its own reason reaches: forward from here is the author's
     * first character, which deletes like any other. */
    g_assert_false(
        editor_delete_leaves_list(FALSE, forward, EDITOR_BLOCK_BULLET_LIST, 2, 2));
    g_assert_true(
        editor_delete_leaves_list(FALSE, FALSE, EDITOR_BLOCK_BULLET_LIST, 2, 2));

    /* And a selection is what a delete over one deletes. */
    g_assert_false(
        editor_delete_leaves_list(TRUE, forward, EDITOR_BLOCK_BULLET_LIST, 2, 0));

    /* The line below, which is the case the caret's own line cannot see. */
    g_assert_true(
        editor_delete_leaves_next_list(FALSE, TRUE, EDITOR_BLOCK_BULLET_LIST, 2));
    g_assert_true(
        editor_delete_leaves_next_list(FALSE, TRUE, EDITOR_BLOCK_NUMBERED_LIST, 3));

    /* Only from the end of a line: anywhere else the newline is not in the
     * range, so no marker crosses. */
    g_assert_false(
        editor_delete_leaves_next_list(FALSE, FALSE, EDITOR_BLOCK_BULLET_LIST, 2));

    /* A plain line below carries nothing that must not cross, and the last line
     * of the buffer has nothing below it at all. */
    g_assert_false(
        editor_delete_leaves_next_list(FALSE, TRUE, EDITOR_BLOCK_PARAGRAPH, 0));
    g_assert_false(
        editor_delete_leaves_next_list(FALSE, TRUE, EDITOR_BLOCK_HEADING_1, 0));
    g_assert_false(editor_delete_leaves_next_list(FALSE, TRUE, EDITOR_BLOCK_OTHER, 0));
    g_assert_false(
        editor_delete_leaves_next_list(FALSE, TRUE, EDITOR_BLOCK_BULLET_LIST, 0));

    /* And a selection is deleted rather than read as a press against a marker. */
    g_assert_false(
        editor_delete_leaves_next_list(TRUE, TRUE, EDITOR_BLOCK_BULLET_LIST, 2));
}

/* The way out has to leave the words behind: taking the item off is a block
 * pick like any other, so what was typed into it stays typed. */
static void test_leaving_a_list_keeps_the_words(void)
{
    Fixture fixture;
    fixture_init(&fixture, "first\nsecond\n");

    set_style(&fixture, 1, 1, EDITOR_BLOCK_BULLET_LIST);
    assert_line(&fixture, 1, "- second");

    set_style(&fixture, 1, 1, EDITOR_BLOCK_PARAGRAPH);
    g_assert_cmpint(style_at(&fixture, 1), ==, EDITOR_BLOCK_PARAGRAPH);
    assert_line(&fixture, 1, "second");

    fixture_clear(&fixture);
}

/* The names and the ids are one table read two ways, and the round trip is
 * what four places agreeing on a style rests on. */
static void test_every_style_has_a_name_and_an_id(void)
{
    for (int style = 0; style < EDITOR_BLOCK_STYLE_COUNT; style++) {
        const char* name = editor_block_style_name((EditorBlockStyle) style);
        const char* id   = editor_block_style_id((EditorBlockStyle) style);
        g_assert_nonnull(name);
        g_assert_nonnull(id);
        g_assert_cmpint(editor_block_style_from_id(id), ==, style);
    }

    /* Anything else is not a style, and must not come back as one. */
    g_assert_null(editor_block_style_name(EDITOR_BLOCK_OTHER));
    g_assert_null(editor_block_style_id(EDITOR_BLOCK_STYLE_COUNT));
    g_assert_cmpint(editor_block_style_from_id("heading-9"), ==, EDITOR_BLOCK_OTHER);
    g_assert_cmpint(editor_block_style_from_id(NULL), ==, EDITOR_BLOCK_OTHER);
}

/* The ids reach the menu items and the accelerators as GAction targets, so an
 * id that needed quoting would break both quietly — the menu item would name an
 * action nothing answers. Every one has to survive the round trip GTK does. */
static void test_every_id_survives_a_detailed_action_name(void)
{
    for (int style = 0; style < EDITOR_BLOCK_STYLE_COUNT; style++) {
        GVariant* target = g_variant_ref_sink(
            g_variant_new_string(editor_block_style_id((EditorBlockStyle) style)));
        char* detailed = g_action_print_detailed_name("win.block-style", target);
        g_variant_unref(target);

        char*     name   = NULL;
        GVariant* parsed = NULL;
        GError*   error  = NULL;
        g_assert_true(g_action_parse_detailed_name(detailed, &name, &parsed, &error));
        g_assert_no_error(error);
        g_assert_cmpstr(name, ==, "win.block-style");
        g_assert_nonnull(parsed);
        g_assert_cmpint(editor_block_style_from_id(g_variant_get_string(parsed, NULL)),
                        ==, style);

        g_free(name);
        g_variant_unref(parsed);
        g_free(detailed);
    }
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/block-style/untagged-is-paragraph",
                    test_untagged_lines_are_paragraphs);
    g_test_add_func("/block-style/a-pick-replaces",
                    test_a_pick_replaces_what_was_there);
    g_test_add_func("/block-style/words-are-left-alone",
                    test_a_pick_leaves_the_words_alone);
    g_test_add_func("/block-style/empty-line", test_an_empty_line_can_carry_a_style);
    g_test_add_func("/block-style/last-line", test_the_last_line_can_carry_a_style);
    g_test_add_func("/block-style/bullet-markers",
                    test_a_bulleted_list_gains_and_loses_its_markers);
    g_test_add_func("/block-style/numbered-counts", test_a_numbered_list_counts);
    g_test_add_func("/block-style/renumbered-whole",
                    test_a_numbered_run_is_renumbered_whole);
    g_test_add_func("/block-style/unoffered-kinds",
                    test_an_unoffered_kind_is_not_a_paragraph);
    g_test_add_func("/block-style/only-lists-have-an-off",
                    test_only_the_lists_have_an_off);
    g_test_add_func("/block-style/return-carries-a-list-on",
                    test_a_return_carries_a_list_on);
    g_test_add_func("/block-style/typed-marker-asks-for-a-list",
                    test_a_typed_marker_asks_for_a_list);
    g_test_add_func("/block-style/converted-line-gives-its-marker-back",
                    test_a_converted_line_gives_its_marker_back);
    g_test_add_func("/block-style/backspace-leaves-a-list",
                    test_a_backspace_leaves_a_list);
    g_test_add_func("/block-style/delete-leaves-a-list", test_a_delete_leaves_a_list);
    g_test_add_func("/block-style/leaving-a-list-keeps-the-words",
                    test_leaving_a_list_keeps_the_words);
    g_test_add_func("/block-style/names-and-ids",
                    test_every_style_has_a_name_and_an_id);
    g_test_add_func("/block-style/ids-as-action-targets",
                    test_every_id_survives_a_detailed_action_name);
    return g_test_run();
}
