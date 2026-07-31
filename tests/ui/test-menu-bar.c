#include <gio/gio.h>

/* A placeholder for the UI test suite. menu-bar.c cannot be pulled in headless
 * yet because menu_bar_new() needs a GtkApplication and a realised window; the
 * menu model builder gets split out and tested here once it has a seam. */
static void test_action_naming(void)
{
    /* Detailed action names in the menu model are all window-scoped, so the
     * "win." prefix has to parse. */
    char* target_prefix = NULL;
    GVariant* target = NULL;
    GError* error = NULL;

    gboolean parsed = g_action_parse_detailed_name(
        "win.open-project", &target_prefix, &target, &error);

    g_assert_true(parsed);
    g_assert_no_error(error);
    g_assert_cmpstr(target_prefix, ==, "win.open-project");

    g_free(target_prefix);
    if (target != NULL) {
        g_variant_unref(target);
    }
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/menu-bar/action-naming", test_action_naming);
    return g_test_run();
}
