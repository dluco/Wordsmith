#include "core/markup-c.h"
#include "ui/editor-panel.h"

#include <gtk/gtk.h>

/* What the format bar shows is editor_style_flags()' answer, so this is where
 * the bar is tested: a GtkTextBuffer and its tags are plain objects, and the
 * two rules the function follows — the whole of a selection, the character
 * behind a bare cursor — are the parts that can be quietly wrong.
 *
 * The other half of the file is the pair of rules underneath typing into a
 * style that is not in the text yet, which is the same answer read forwards. */

/* The tags in bit order: inline_tags[n] answers for 1 << n, which is the order
 * the WORDSMITH_MARKUP_SPAN_* flags are declared in. The editor builds the same
 * four; these are its stand-ins. */
#define TAG_COUNT 4

typedef struct Fixture {
    GtkTextBuffer* buffer;
    GtkTextTag*    tags[TAG_COUNT];
} Fixture;

static void fixture_init(Fixture* fixture, const char* text)
{
    fixture->buffer = gtk_text_buffer_new(NULL);
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

static void apply(Fixture* fixture, int bit, int from, int to)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &start, from);
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &end, to);
    gtk_text_buffer_apply_tag(fixture->buffer, fixture->tags[bit], &start, &end);
}

static void place_cursor(Fixture* fixture, int offset)
{
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &at, offset);
    gtk_text_buffer_place_cursor(fixture->buffer, &at);
}

static void select_range(Fixture* fixture, int from, int to)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &start, from);
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &end, to);
    gtk_text_buffer_select_range(fixture->buffer, &start, &end);
}

static uint32_t flags(Fixture* fixture)
{
    return editor_style_flags(fixture->buffer, fixture->tags, TAG_COUNT);
}

/* Walking through bold text is the whole point of the bar following the
 * cursor: the button lights on the way in and goes out on the way past. */
static void test_the_cursor_reads_the_text_behind_it(void)
{
    Fixture fixture;
    fixture_init(&fixture, "plain bold plain");
    apply(&fixture, 1, 6, 10);   /* "bold" */

    place_cursor(&fixture, 3);
    g_assert_cmpuint(flags(&fixture), ==, 0);

    place_cursor(&fixture, 8);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    /* Sitting just past the last bold character. The character behind is still
     * bold, and it is the one the author would say they are in — reading the
     * character ahead instead would put the button out one keystroke early. */
    place_cursor(&fixture, 10);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    /* One further on, and the run is behind them. */
    place_cursor(&fixture, 11);
    g_assert_cmpuint(flags(&fixture), ==, 0);

    /* Standing on the first bold character, with plain text behind. */
    place_cursor(&fixture, 6);
    g_assert_cmpuint(flags(&fixture), ==, 0);

    fixture_clear(&fixture);
}

/* At the start of a line there is nothing behind the cursor — the newline
 * belongs to the line above, and its styles are not this line's business. */
static void test_the_start_of_a_line_reads_ahead(void)
{
    Fixture fixture;
    fixture_init(&fixture, "bold line\nplain line");
    apply(&fixture, 1, 0, 9);

    place_cursor(&fixture, 0);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    /* The first character of the second line: the bold run ends on the line
     * above and must not follow the author down onto this one. */
    place_cursor(&fixture, 10);
    g_assert_cmpuint(flags(&fixture), ==, 0);

    fixture_clear(&fixture);
}

/* Styles stack, and the bar shows all of them. */
static void test_stacked_styles_all_report(void)
{
    Fixture fixture;
    fixture_init(&fixture, "shouting quietly");
    apply(&fixture, 1, 0, 8);   /* strong */
    apply(&fixture, 0, 0, 8);   /* emphasis */

    place_cursor(&fixture, 4);
    g_assert_cmpuint(flags(&fixture), ==,
                     WORDSMITH_MARKUP_SPAN_STRONG | WORDSMITH_MARKUP_SPAN_EMPHASIS);

    fixture_clear(&fixture);
}

/* Over a selection the answer is the toggle's own rule, so a lit button always
 * means "click to take this off". A selection that is only partly bold is not
 * bold: clicking would make all of it bold rather than none. */
static void test_a_selection_reports_only_what_covers_it(void)
{
    Fixture fixture;
    fixture_init(&fixture, "bold plain");
    apply(&fixture, 1, 0, 4);

    select_range(&fixture, 0, 4);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    select_range(&fixture, 0, 7);
    g_assert_cmpuint(flags(&fixture), ==, 0);

    select_range(&fixture, 1, 3);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    /* Backwards, which is what dragging right to left leaves behind. The
     * bounds come back ordered, so it is the same answer. */
    select_range(&fixture, 4, 0);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    fixture_clear(&fixture);
}

/* A run that reaches the last character of the buffer has no toggle after it to
 * stop at, which is the one case the covering test answers from the end of the
 * buffer rather than from a tag. */
static void test_a_run_to_the_end_of_the_buffer(void)
{
    Fixture fixture;
    fixture_init(&fixture, "all bold");
    apply(&fixture, 1, 0, 8);

    select_range(&fixture, 0, 8);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    place_cursor(&fixture, 8);
    g_assert_cmpuint(flags(&fixture), ==, WORDSMITH_MARKUP_SPAN_STRONG);

    fixture_clear(&fixture);
}

/* An empty document, and a buffer with no tags at all. Neither may report a
 * style, and neither may walk off the end of the buffer looking for one. */
static void test_an_empty_buffer_reports_nothing(void)
{
    Fixture fixture;
    fixture_init(&fixture, "");

    place_cursor(&fixture, 0);
    g_assert_cmpuint(flags(&fixture), ==, 0);

    g_assert_cmpuint(editor_style_flags(fixture.buffer, NULL, TAG_COUNT), ==, 0);
    g_assert_cmpuint(editor_style_flags(NULL, fixture.tags, TAG_COUNT), ==, 0);

    /* Fewer tags than the buffer has: the count is what is asked about, so a
     * style nobody passed in cannot light a button. */
    apply(&fixture, 1, 0, 0);
    g_assert_cmpuint(editor_style_flags(fixture.buffer, fixture.tags, 0), ==, 0);

    fixture_clear(&fixture);
}

/* With nothing asked for, text takes the styling of the place it lands in.
 * That is the case that makes carrying on at the end of a bold word work,
 * where GtkTextBuffer's own rule would hand back plain text. */
static void test_typed_text_follows_the_text_beside_it(void)
{
    g_assert_cmpuint(editor_typed_styles(0, 0, 0), ==, 0);
    g_assert_cmpuint(editor_typed_styles(WORDSMITH_MARKUP_SPAN_STRONG, 0, 0), ==,
                     WORDSMITH_MARKUP_SPAN_STRONG);

    const uint32_t both =
        WORDSMITH_MARKUP_SPAN_STRONG | WORDSMITH_MARKUP_SPAN_EMPHASIS;
    g_assert_cmpuint(editor_typed_styles(both, 0, 0), ==, both);
}

/* What was asked for wins, in both directions. The "off" direction is the one
 * a single word of bits could not express: turning bold off at the end of a
 * bold word has to beat the bold character behind the cursor. */
static void test_what_was_asked_for_overrules_it(void)
{
    /* Bold asked for where there is none. */
    g_assert_cmpuint(editor_typed_styles(0, WORDSMITH_MARKUP_SPAN_STRONG,
                                         WORDSMITH_MARKUP_SPAN_STRONG),
                     ==, WORDSMITH_MARKUP_SPAN_STRONG);

    /* Bold turned off where the text is bold. */
    g_assert_cmpuint(editor_typed_styles(WORDSMITH_MARKUP_SPAN_STRONG,
                                         WORDSMITH_MARKUP_SPAN_STRONG, 0),
                     ==, 0);

    /* An answer about one style says nothing about the others, which carry on
     * from the text: italic here is not disturbed by either answer about bold. */
    g_assert_cmpuint(editor_typed_styles(WORDSMITH_MARKUP_SPAN_EMPHASIS,
                                         WORDSMITH_MARKUP_SPAN_STRONG,
                                         WORDSMITH_MARKUP_SPAN_STRONG),
                     ==,
                     WORDSMITH_MARKUP_SPAN_EMPHASIS | WORDSMITH_MARKUP_SPAN_STRONG);
    g_assert_cmpuint(editor_typed_styles(WORDSMITH_MARKUP_SPAN_EMPHASIS,
                                         WORDSMITH_MARKUP_SPAN_STRONG, 0),
                     ==, WORDSMITH_MARKUP_SPAN_EMPHASIS);

    /* A stale flag outside the mask is not an answer and changes nothing. */
    g_assert_cmpuint(editor_typed_styles(0, 0, WORDSMITH_MARKUP_SPAN_STRONG), ==, 0);
}

/* Pressing Bold means "the other thing from what I am standing in", so the same
 * press asks for bold in plain text and for the end of it in bold text. */
static void test_a_press_asks_for_the_other_thing(void)
{
    uint32_t mask  = 0;
    uint32_t flags = 0;

    editor_ask_for_style(WORDSMITH_MARKUP_SPAN_STRONG, 0, &mask, &flags);
    g_assert_cmpuint(mask, ==, WORDSMITH_MARKUP_SPAN_STRONG);
    g_assert_cmpuint(editor_typed_styles(0, mask, flags), ==,
                     WORDSMITH_MARKUP_SPAN_STRONG);

    /* Pressing it again, now that bold is what is in force, puts it back. The
     * mask keeps the answer: "not bold" is a thing the author has said, not a
     * thing they have stopped saying. */
    editor_ask_for_style(WORDSMITH_MARKUP_SPAN_STRONG,
                         WORDSMITH_MARKUP_SPAN_STRONG, &mask, &flags);
    g_assert_cmpuint(mask, ==, WORDSMITH_MARKUP_SPAN_STRONG);
    g_assert_cmpuint(editor_typed_styles(WORDSMITH_MARKUP_SPAN_STRONG, mask, flags),
                     ==, 0);

    /* A second style stacks beside the first rather than replacing it: Ctrl+B,
     * Ctrl+I, then type, comes out bold and italic. */
    editor_ask_for_style(WORDSMITH_MARKUP_SPAN_STRONG, 0, &mask, &flags);
    editor_ask_for_style(WORDSMITH_MARKUP_SPAN_EMPHASIS, 0, &mask, &flags);
    g_assert_cmpuint(editor_typed_styles(0, mask, flags), ==,
                     WORDSMITH_MARKUP_SPAN_STRONG | WORDSMITH_MARKUP_SPAN_EMPHASIS);

    /* Somewhere to put the answer is the one thing it needs. */
    editor_ask_for_style(WORDSMITH_MARKUP_SPAN_STRONG, 0, NULL, NULL);
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/format-bar/cursor-reads-behind",
                    test_the_cursor_reads_the_text_behind_it);
    g_test_add_func("/format-bar/line-start-reads-ahead",
                    test_the_start_of_a_line_reads_ahead);
    g_test_add_func("/format-bar/stacked-styles", test_stacked_styles_all_report);
    g_test_add_func("/format-bar/selection-must-be-covered",
                    test_a_selection_reports_only_what_covers_it);
    g_test_add_func("/format-bar/run-to-the-end",
                    test_a_run_to_the_end_of_the_buffer);
    g_test_add_func("/format-bar/empty-buffer", test_an_empty_buffer_reports_nothing);
    g_test_add_func("/typing/follows-the-text",
                    test_typed_text_follows_the_text_beside_it);
    g_test_add_func("/typing/asked-for-overrules",
                    test_what_was_asked_for_overrules_it);
    g_test_add_func("/typing/press-asks-for-the-other-thing",
                    test_a_press_asks_for_the_other_thing);
    return g_test_run();
}
