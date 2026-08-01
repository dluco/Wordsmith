#include "ui/text-scale.h"

#include "core/preferences-c.h"

#include <gtk/gtk.h>

#include <string.h>

/* GtkCssProvider parses without a display, so the rule the editor's text size
 * rides on can be checked here rather than by eye. */

static void on_parsing_error(GtkCssProvider* provider, GtkCssSection* section,
                             const GError* error, gpointer user_data)
{
    (void) provider;
    (void) section;

    int* seen = user_data;
    (*seen)++;
    g_test_message("CSS parse error: %s", error->message);
}

static int parse_errors_in(const char* css)
{
    int seen = 0;

    GtkCssProvider* provider = gtk_css_provider_new();
    g_signal_connect(provider, "parsing-error", G_CALLBACK(on_parsing_error), &seen);
    gtk_css_provider_load_from_string(provider, css);
    g_object_unref(provider);

    return seen;
}

static void test_rule_parses(void)
{
    char* css = text_scale_css(WORDSMITH_TEXT_SCALE_DEFAULT_PERCENT);

    g_assert_cmpint(parse_errors_in(css), ==, 0);
    g_assert_nonnull(strstr(css, ".editor-pane textview"));
    g_assert_nonnull(strstr(css, "font-size: 100%"));

    g_free(css);
}

/* Only the editor pane grows: the binder and inspector are chrome, and a
 * stylesheet that scaled everything would be a different feature. */
static void test_rule_is_scoped_to_the_editor(void)
{
    char* css = text_scale_css(150);

    g_assert_null(strstr(css, "window"));
    g_assert_null(strstr(css, "label"));
    g_assert_nonnull(strstr(css, "font-size: 150%"));

    g_free(css);
}

static void test_out_of_range_sizes_are_clamped(void)
{
    char* enormous = text_scale_css(WORDSMITH_TEXT_SCALE_MAX_PERCENT + 500);
    char* tiny = text_scale_css(WORDSMITH_TEXT_SCALE_MIN_PERCENT - 500);
    char* negative = text_scale_css(-100);

    char* expected_max =
        g_strdup_printf("font-size: %d%%", WORDSMITH_TEXT_SCALE_MAX_PERCENT);
    char* expected_min =
        g_strdup_printf("font-size: %d%%", WORDSMITH_TEXT_SCALE_MIN_PERCENT);

    g_assert_nonnull(strstr(enormous, expected_max));
    g_assert_nonnull(strstr(tiny, expected_min));
    g_assert_nonnull(strstr(negative, expected_min));

    /* A negative size would parse as a broken rule rather than a small one. */
    g_assert_cmpint(parse_errors_in(negative), ==, 0);

    g_free(expected_max);
    g_free(expected_min);
    g_free(enormous);
    g_free(tiny);
    g_free(negative);
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/text-scale/rule-parses", test_rule_parses);
    g_test_add_func("/text-scale/scoped-to-editor", test_rule_is_scoped_to_the_editor);
    g_test_add_func("/text-scale/clamped", test_out_of_range_sizes_are_clamped);
    return g_test_run();
}
