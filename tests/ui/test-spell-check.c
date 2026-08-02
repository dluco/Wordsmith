#include "ui/spell-check.h"

#include <gtk/gtk.h>

/* Marking misspelled words needs a buffer and its tags, both plain objects, and
 * no display — the same seam the format bar and the undo history are tested
 * through.
 *
 * Two halves. The cursor rule is arithmetic and always runs. The marking itself
 * needs a dictionary installed, and there is no promising one: a machine
 * without any is exactly the case where nothing may be marked, so those tests
 * check that and then skip the rest rather than failing over the machine they
 * are on. */

typedef struct Fixture {
    GtkTextBuffer* buffer;
    SpellCheck*    spelling;
} Fixture;

static void fixture_init(Fixture* fixture, const char* text)
{
    fixture->buffer   = gtk_text_buffer_new(NULL);
    fixture->spelling = spell_check_new(fixture->buffer);
    spell_check_set_enabled(fixture->spelling, TRUE);

    gtk_text_buffer_set_text(fixture->buffer, text, -1);
    spell_check_refresh(fixture->spelling);
}

static void fixture_clear(Fixture* fixture)
{
    spell_check_free(fixture->spelling);
    g_object_unref(fixture->buffer);
}

/* Whether every character of [from, to) is marked, and nothing either side of
 * it is — a mark under half a word is as wrong as no mark at all. */
static gboolean only_marked(Fixture* fixture, int from, int to)
{
    GtkTextTag* tag = gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(fixture->buffer), "misspelled");
    g_assert_nonnull(tag);

    GtkTextIter end_of_buffer;
    gtk_text_buffer_get_end_iter(fixture->buffer, &end_of_buffer);

    for (int offset = 0; offset < gtk_text_iter_get_offset(&end_of_buffer); offset++) {
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(fixture->buffer, &at, offset);

        const gboolean marked = gtk_text_iter_has_tag(&at, tag);
        const gboolean wanted = offset >= from && offset < to;
        if (marked != wanted) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean nothing_marked(Fixture* fixture)
{
    return only_marked(fixture, 0, 0);
}

static void place_cursor(Fixture* fixture, int offset)
{
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(fixture->buffer, &at, offset);
    gtk_text_buffer_place_cursor(fixture->buffer, &at);
}

/* Whether there is a dictionary to ask. Without one the checker knows every
 * word, which is the contract — and leaves nothing for the rest to look at. */
static gboolean dictionary_installed(void)
{
    GtkTextBuffer* buffer   = gtk_text_buffer_new(NULL);
    SpellCheck*    spelling = spell_check_new(buffer);
    const gboolean found    = spell_check_available(spelling);

    spell_check_free(spelling);
    g_object_unref(buffer);
    return found;
}

/* ── the cursor rule ─────────────────────────────────────────────────────── */

/* The trailing edge counts and the leading one does not, and both halves of
 * that matter. The cursor sits at the end of a word for as long as it takes to
 * type one; it sits at the start of one without anything having been typed
 * there, which is where deleting the word in front of it leaves it. */
static void test_the_word_being_typed_is_the_one_at_the_cursor(void)
{
    /* "hello" at offsets 4 to 9, with the text last edited where the cursor
     * stands — which is what typing it looks like. */
    g_assert_true(spell_check_word_being_typed(9, 9, 4, 9));  /* typed the "o" */
    g_assert_true(spell_check_word_being_typed(6, 6, 4, 9));  /* inside it */
    g_assert_true(spell_check_word_being_typed(5, 5, 4, 9));  /* one character in */

    g_assert_false(spell_check_word_being_typed(4, 4, 4, 9));   /* in front of it */
    g_assert_false(spell_check_word_being_typed(10, 10, 4, 9)); /* past the end */
    g_assert_false(spell_check_word_being_typed(0, 0, 4, 9));   /* nowhere near */
}

/* A cursor that arrived rather than wrote leaves the word alone. Clicking into
 * a misspelling is the author asking about the mark, and taking it off under
 * the pointer answers by hiding the question. */
static void test_a_cursor_that_typed_nothing_leaves_the_mark(void)
{
    g_assert_false(spell_check_word_being_typed(6, 22, 4, 9));  /* clicked into it */
    g_assert_false(spell_check_word_being_typed(9, -1, 4, 9));  /* nothing typed yet */
}

/* The top of a document is offset 0, so a word starting there must still be
 * checked: counting the leading edge would leave the first word of every
 * manuscript as the one word never looked at. */
static void test_the_first_word_of_a_document_is_checked(void)
{
    g_assert_false(spell_check_word_being_typed(0, 0, 0, 5));
}

/* ── the marking ─────────────────────────────────────────────────────────── */

static void test_a_misspelling_is_marked(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    Fixture fixture;
    fixture_init(&fixture, "the qwertzuiop sat");

    g_assert_true(only_marked(&fixture, 4, 14));
    fixture_clear(&fixture);
}

static void test_ordinary_prose_is_left_alone(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    Fixture fixture;
    fixture_init(&fixture, "the cat sat on the mat");

    g_assert_true(nothing_marked(&fixture));
    fixture_clear(&fixture);
}

/* Turning it off takes the marks off the page rather than leaving them where
 * they were, and turning it back on puts them back without a document having
 * to be reopened. */
static void test_turning_it_off_clears_the_page(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    Fixture fixture;
    fixture_init(&fixture, "the qwertzuiop sat");
    g_assert_true(only_marked(&fixture, 4, 14));

    spell_check_set_enabled(fixture.spelling, FALSE);
    g_assert_true(nothing_marked(&fixture));

    spell_check_set_enabled(fixture.spelling, TRUE);
    g_assert_true(only_marked(&fixture, 4, 14));

    fixture_clear(&fixture);
}

/* Typing does not mark the word being typed, and leaving it does. This is the
 * whole reason the cursor rule exists, so it is checked through the buffer and
 * not only through the arithmetic. */
static void test_a_word_is_marked_once_it_is_left(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    Fixture fixture;
    fixture_init(&fixture, "");

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(fixture.buffer, &end);
    gtk_text_buffer_insert(fixture.buffer, &end, "qwertzuiop", -1);

    /* The cursor is still at the end of it, so it is a word in progress. */
    g_assert_true(nothing_marked(&fixture));

    /* A space finishes it. */
    gtk_text_buffer_get_end_iter(fixture.buffer, &end);
    gtk_text_buffer_insert(fixture.buffer, &end, " ", -1);
    g_assert_true(only_marked(&fixture, 0, 10));

    fixture_clear(&fixture);
}

/* An edit that joins two words has to be read as one word, which is why a
 * whole line is rechecked rather than the characters that changed. */
static void test_an_edit_rechecks_the_whole_line(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    Fixture fixture;
    fixture_init(&fixture, "the cat sat");
    g_assert_true(nothing_marked(&fixture));

    /* Delete the space between "cat" and "sat", making "catsat" of them. */
    GtkTextIter from;
    GtkTextIter to;
    gtk_text_buffer_get_iter_at_offset(fixture.buffer, &from, 7);
    gtk_text_buffer_get_iter_at_offset(fixture.buffer, &to, 8);
    gtk_text_buffer_delete(fixture.buffer, &from, &to);

    /* The cursor lands where the space was, which is inside the new word, so
     * it is left alone until it is left. */
    place_cursor(&fixture, 0);
    g_assert_true(only_marked(&fixture, 4, 10));

    fixture_clear(&fixture);
}

/* A dictionary has no opinion about code, and marking it would be noise. */
static void test_text_wearing_a_skipped_tag_is_not_checked(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    GtkTextBuffer* buffer   = gtk_text_buffer_new(NULL);
    GtkTextTag*    code     = gtk_text_buffer_create_tag(buffer, "code-span", NULL);
    SpellCheck*    spelling = spell_check_new(buffer);

    spell_check_skip_tag(spelling, code);
    spell_check_set_enabled(spelling, TRUE);
    gtk_text_buffer_set_text(buffer, "call qwertzuiop now", -1);

    GtkTextIter from;
    GtkTextIter to;
    gtk_text_buffer_get_iter_at_offset(buffer, &from, 5);
    gtk_text_buffer_get_iter_at_offset(buffer, &to, 15);
    gtk_text_buffer_apply_tag(buffer, code, &from, &to);

    spell_check_refresh(spelling);

    Fixture fixture = { buffer, spelling };
    g_assert_true(nothing_marked(&fixture));

    fixture_clear(&fixture);
}

/* The bug the cursor rule caused before it asked where the text was last
 * edited: clicking on a marked word took its mark off, so the one gesture an
 * author makes to ask about a misspelling made it disappear. */
static void test_clicking_a_marked_word_keeps_its_mark(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    Fixture fixture;
    fixture_init(&fixture, "");

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(fixture.buffer, &end);
    gtk_text_buffer_insert(fixture.buffer, &end, "qwertzuiop here", -1);

    /* Away from it, so it is finished and marked. */
    place_cursor(&fixture, 15);
    g_assert_true(only_marked(&fixture, 0, 10));

    /* And back into the middle of it, the way a click does. */
    place_cursor(&fixture, 5);
    g_assert_true(only_marked(&fixture, 0, 10));

    /* Typing there is a word in progress again, and the mark comes off. */
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(fixture.buffer, &at, 5);
    gtk_text_buffer_insert(fixture.buffer, &at, "x", -1);
    g_assert_true(nothing_marked(&fixture));

    fixture_clear(&fixture);
}

/* ── the menu over a misspelling ─────────────────────────────────────────── */

/* A GMenuModel is a GLib object, so what a right click offers can be read
 * without a display. What cannot be is the gesture that shows it, which is why
 * the model is built apart from it. */
static GMenuModel* section_of(GMenuModel* menu, int index)
{
    GMenuModel* section = g_menu_model_get_item_link(menu, index, G_MENU_LINK_SECTION);
    g_assert_nonnull(section);
    return section;
}

static void test_the_menu_offers_the_corrections(void)
{
    const char* const corrections[] = { "hello", "hallo", "hell" };
    GMenu*      filled = g_menu_new();
    GMenuModel* menu   = G_MENU_MODEL(filled);
    spell_check_fill_menu(filled, "helo", corrections, 3);

    /* The word it is about, then the corrections, then the dictionary. */
    g_assert_cmpint(g_menu_model_get_n_items(menu), ==, 2);

    char* heading = NULL;
    g_assert_true(g_menu_model_get_item_attribute(menu, 0, G_MENU_ATTRIBUTE_LABEL,
                                                  "s", &heading));
    g_assert_cmpstr(heading, ==, "helo");
    g_free(heading);

    GMenuModel* offered = section_of(menu, 0);
    g_assert_cmpint(g_menu_model_get_n_items(offered), ==, 3);

    for (int index = 0; index < 3; index++) {
        char* label = NULL;
        char* action = NULL;
        GVariant* target = NULL;

        g_assert_true(g_menu_model_get_item_attribute(offered, index,
                                                      G_MENU_ATTRIBUTE_LABEL, "s",
                                                      &label));
        g_assert_true(g_menu_model_get_item_attribute(offered, index,
                                                      G_MENU_ATTRIBUTE_ACTION, "s",
                                                      &action));
        target = g_menu_model_get_item_attribute_value(
            offered, index, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE_STRING);

        g_assert_cmpstr(label, ==, corrections[index]);
        g_assert_cmpstr(action, ==, "spelling.correct");
        g_assert_nonnull(target);
        /* The replacement travels with the item, so nothing has to be kept on
         * the side and looked up again when it is chosen. */
        g_assert_cmpstr(g_variant_get_string(target, NULL), ==, corrections[index]);

        g_free(label);
        g_free(action);
        g_variant_unref(target);
    }

    GMenuModel* dictionary = section_of(menu, 1);
    g_assert_cmpint(g_menu_model_get_n_items(dictionary), ==, 2);

    g_object_unref(offered);
    g_object_unref(dictionary);
    g_object_unref(menu);
}

/* A right click that opens nothing looks like a fault, so a word the dictionary
 * has no idea about still gets a menu that says so. */
static void test_a_word_with_nothing_to_offer_still_gets_a_menu(void)
{
    GMenu* filled = g_menu_new();
    spell_check_fill_menu(filled, "qwertzuiop", NULL, 0);

    GMenuModel* menu    = G_MENU_MODEL(filled);
    GMenuModel* offered = section_of(menu, 0);

    g_assert_cmpint(g_menu_model_get_n_items(offered), ==, 1);

    char* action = NULL;
    g_assert_true(g_menu_model_get_item_attribute(offered, 0, G_MENU_ATTRIBUTE_ACTION,
                                                  "s", &action));
    /* An action that is never enabled, so the item is greyed rather than gone. */
    g_assert_cmpstr(action, ==, "spelling.no-suggestions");
    g_free(action);

    /* And a word can still be accepted, which is the point of the menu for a
     * character's name. */
    GMenuModel* dictionary = section_of(menu, 1);
    g_assert_cmpint(g_menu_model_get_n_items(dictionary), ==, 2);

    g_object_unref(offered);
    g_object_unref(dictionary);
    g_object_unref(menu);
}

/* A dictionary will give twenty variations on one typo. A menu is a list to be
 * read at a glance, so only the first few are offered. */
static void test_only_the_first_corrections_are_offered(void)
{
    const char* const corrections[] = {
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    };
    GMenu* filled = g_menu_new();
    spell_check_fill_menu(filled, "x", corrections, G_N_ELEMENTS(corrections));

    GMenuModel* menu    = G_MENU_MODEL(filled);
    GMenuModel* offered = section_of(menu, 0);

    g_assert_cmpint(g_menu_model_get_n_items(offered), ==, 8);

    g_object_unref(offered);
    g_object_unref(menu);
}

/* The same menu is handed to the view once and filled from then on — GTK builds
 * its popover from the model and follows it, and handing over another would
 * throw that popover away. So filling it has to replace what a previous word
 * left rather than pile on top of it. */
static void test_filling_the_menu_replaces_what_was_there(void)
{
    const char* const first[]  = { "hello", "hallo" };
    const char* const second[] = { "cat" };

    GMenu* menu = g_menu_new();
    spell_check_fill_menu(menu, "helo", first, 2);
    spell_check_fill_menu(menu, "cta", second, 1);

    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(menu)), ==, 2);

    char* heading = NULL;
    g_assert_true(g_menu_model_get_item_attribute(G_MENU_MODEL(menu), 0,
                                                  G_MENU_ATTRIBUTE_LABEL, "s",
                                                  &heading));
    g_assert_cmpstr(heading, ==, "cta");
    g_free(heading);

    GMenuModel* offered = section_of(G_MENU_MODEL(menu), 0);
    g_assert_cmpint(g_menu_model_get_n_items(offered), ==, 1);

    g_object_unref(offered);
    g_object_unref(menu);
}

/* A held marker follows nothing, and releasing it checks the document once —
 * which is how a load of several hundred insertions costs one pass. */
static void test_a_hold_defers_the_marking_to_its_release(void)
{
    if (!dictionary_installed()) {
        g_test_skip("no dictionary installed");
        return;
    }

    Fixture fixture;
    fixture_init(&fixture, "");

    spell_check_hold(fixture.spelling);
    spell_check_hold(fixture.spelling);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(fixture.buffer, &end);
    gtk_text_buffer_insert(fixture.buffer, &end, "qwertzuiop here", -1);
    g_assert_true(nothing_marked(&fixture));

    /* They nest: the first release is still inside the first hold. */
    spell_check_release(fixture.spelling);
    g_assert_true(nothing_marked(&fixture));

    spell_check_release(fixture.spelling);
    g_assert_true(only_marked(&fixture, 0, 10));

    fixture_clear(&fixture);
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/spell-check/cursor-holds-the-word-being-typed",
                    test_the_word_being_typed_is_the_one_at_the_cursor);
    g_test_add_func("/spell-check/a-cursor-that-typed-nothing-leaves-the-mark",
                    test_a_cursor_that_typed_nothing_leaves_the_mark);
    g_test_add_func("/spell-check/first-word-is-checked",
                    test_the_first_word_of_a_document_is_checked);
    g_test_add_func("/spell-check/marks-a-misspelling", test_a_misspelling_is_marked);
    g_test_add_func("/spell-check/leaves-prose-alone", test_ordinary_prose_is_left_alone);
    g_test_add_func("/spell-check/off-clears-the-page",
                    test_turning_it_off_clears_the_page);
    g_test_add_func("/spell-check/marks-a-word-once-left",
                    test_a_word_is_marked_once_it_is_left);
    g_test_add_func("/spell-check/an-edit-rechecks-the-line",
                    test_an_edit_rechecks_the_whole_line);
    g_test_add_func("/spell-check/skipped-tags-are-not-checked",
                    test_text_wearing_a_skipped_tag_is_not_checked);
    g_test_add_func("/spell-check/hold-defers-to-release",
                    test_a_hold_defers_the_marking_to_its_release);
    g_test_add_func("/spell-check/clicking-a-marked-word-keeps-its-mark",
                    test_clicking_a_marked_word_keeps_its_mark);
    g_test_add_func("/spell-check/menu-offers-the-corrections",
                    test_the_menu_offers_the_corrections);
    g_test_add_func("/spell-check/menu-says-when-there-is-nothing-to-offer",
                    test_a_word_with_nothing_to_offer_still_gets_a_menu);
    g_test_add_func("/spell-check/menu-offers-only-the-first-corrections",
                    test_only_the_first_corrections_are_offered);
    g_test_add_func("/spell-check/filling-the-menu-replaces-it",
                    test_filling_the_menu_replaces_what_was_there);
    return g_test_run();
}
