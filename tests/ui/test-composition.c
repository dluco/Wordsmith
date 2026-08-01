#include "ui/editor-panel.h"

#include <gtk/gtk.h>

/* Composition mode centres the text by computing the view's side margins, so
 * the arithmetic is the part that can be wrong without anyone noticing until
 * they look at it. It needs no display, which is why it is a function rather
 * than a few lines inside the resize handler. */

static void test_a_wide_pane_centres_the_column(void)
{
    /* 1920 wide, a 700pt column: 610 either side puts it in the middle. */
    g_assert_cmpint(editor_composition_margin(1920, 700), ==, 610);
    g_assert_cmpint(editor_composition_margin(1500, 700), ==, 400);

    /* Whatever the width, the column is what is left between the margins —
     * give or take the odd pixel an odd width cannot split, which the next
     * test pins down. */
    for (int width = 900; width <= 3840; width += 137) {
        const int margin = editor_composition_margin(width, 700);
        g_assert_cmpint(width - 2 * margin, >=, 700);
        g_assert_cmpint(width - 2 * margin, <=, 701);
    }
}

/* An odd leftover cannot be split evenly, and a pixel has to go somewhere. It
 * goes to the column rather than to one margin, so the text stays centred
 * rather than sitting one pixel off. */
static void test_an_odd_width_stays_centred(void)
{
    const int margin = editor_composition_margin(1001, 700);

    g_assert_cmpint(margin, ==, 150);
    g_assert_cmpint(1001 - 2 * margin, ==, 701);
}

/* A window too narrow for the column would want a negative margin. It gets the
 * ordinary one instead: the text runs narrower than the column rather than off
 * the edge of the pane. */
static void test_a_narrow_pane_falls_back(void)
{
    g_assert_cmpint(editor_composition_margin(700, 700), ==, EDITOR_SIDE_MARGIN);
    g_assert_cmpint(editor_composition_margin(400, 700), ==, EDITOR_SIDE_MARGIN);
    g_assert_cmpint(editor_composition_margin(0, 700), ==, EDITOR_SIDE_MARGIN);

    /* The width before the first allocation, and the width GTK reports for a
     * widget that has never been shown. Neither may produce a negative margin,
     * which GtkTextView would reject with a warning. */
    g_assert_cmpint(editor_composition_margin(-1, 700), ==, EDITOR_SIDE_MARGIN);

    /* The changeover is where the column plus the ordinary margins fit. */
    const int fits = 700 + 2 * EDITOR_SIDE_MARGIN;
    g_assert_cmpint(editor_composition_margin(fits - 2, 700), ==, EDITOR_SIDE_MARGIN);
    g_assert_cmpint(editor_composition_margin(fits + 2, 700), >, EDITOR_SIDE_MARGIN);
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/composition/wide-pane-centres",
                    test_a_wide_pane_centres_the_column);
    g_test_add_func("/composition/odd-width-stays-centred",
                    test_an_odd_width_stays_centred);
    g_test_add_func("/composition/narrow-pane-falls-back",
                    test_a_narrow_pane_falls_back);
    return g_test_run();
}
