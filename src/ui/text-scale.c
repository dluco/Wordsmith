#include "text-scale.h"

#include "core/preferences-c.h"

#include <gtk/gtk.h>

/* The provider stays alive for the life of the display and is reloaded in
 * place, so the rule it carries is replaced rather than stacked. */
static GtkCssProvider* scale_provider = NULL;
static int             scale_percent = WORDSMITH_TEXT_SCALE_DEFAULT_PERCENT;

static int clamp_percent(int percent)
{
    if (percent < WORDSMITH_TEXT_SCALE_MIN_PERCENT) {
        return WORDSMITH_TEXT_SCALE_MIN_PERCENT;
    }
    if (percent > WORDSMITH_TEXT_SCALE_MAX_PERCENT) {
        return WORDSMITH_TEXT_SCALE_MAX_PERCENT;
    }
    return percent;
}

char* text_scale_css(int percent)
{
    /* The editor pane is the scrolled window; the text view is the node inside
     * it that carries the font, and the size inherits down to the text node
     * from there. A percentage resolves against the font the pane would
     * otherwise use, which is the system font. */
    return g_strdup_printf(".editor-pane textview { font-size: %d%%; }\n",
                           clamp_percent(percent));
}

/* The rule lands one step above the application stylesheet so it wins against
 * anything style.css says about the editor's font, and still loses to a user
 * stylesheet. */
static void apply_percent(int percent)
{
    GdkDisplay* display = gdk_display_get_default();
    if (display == NULL) {
        return;
    }

    if (scale_provider == NULL) {
        scale_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(scale_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }

    char* css = text_scale_css(percent);
    gtk_css_provider_load_from_string(scale_provider, css);
    g_free(css);
}

void text_scale_init(void)
{
    scale_percent = clamp_percent(wordsmith_preferences_text_scale());
    apply_percent(scale_percent);
}

int text_scale_percent(void)
{
    return scale_percent;
}

int text_scale_set(int percent, char** error)
{
    const int wanted = clamp_percent(percent);
    if (wanted == scale_percent) {
        /* Stepping past a bound, or resetting a size that is already the
         * default: nothing to redraw and nothing to write. */
        return 1;
    }

    scale_percent = wanted;
    apply_percent(scale_percent);
    return wordsmith_preferences_set_text_scale(scale_percent, error);
}
