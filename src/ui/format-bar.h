#pragma once

#include <gtk/gtk.h>
#include <stdint.h>

/* The strip of formatting buttons above the manuscript.
 *
 * GTK4 has no control for this. GtkToolbar and GtkToolButton went out with
 * GTK3, GtkActionBar is a contextual strip meant for the bottom of a window,
 * and GtkTextView ships nothing of its own — so a format bar anywhere in GTK4
 * is a GtkBox carrying the `.toolbar` style class with ordinary buttons inside
 * it. That is what this is, and the stylesheet does the rest.
 *
 * The buttons only *name* the window's format actions, the way the binder's
 * context menu names win.new-text-in: the bar has no idea there is an editor,
 * and Ctrl+B, the Format menu and this button are three ways into one verb
 * rather than three implementations of it.
 *
 * The buttons follow the text, never the click. A press raises the action and
 * nothing else; what lights the button up is the editor reporting back what is
 * in force where the author is standing, through format_bar_show_styles(). With
 * nothing selected that is a style waiting for the next thing typed, so the
 * button lights before the text exists — but it is still the editor's answer
 * being drawn, never the press. A press the editor does nothing with, with no
 * document open, therefore leaves the button where it was. */
typedef struct FormatBar FormatBar;

FormatBar* format_bar_new(void);
void       format_bar_free(FormatBar* bar);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* format_bar_widget(FormatBar* bar);

/** Light the buttons for the styles in `flags`, a set of
 *  WORDSMITH_MARKUP_SPAN_* bits, and put out the rest. This is the only thing
 *  that moves a button: see the note above. */
void format_bar_show_styles(FormatBar* bar, uint32_t flags);

/** Put the bar on screen, or away. Hiding it leaves the editor the height it
 *  gives up. */
void     format_bar_set_visible(FormatBar* bar, gboolean visible);
gboolean format_bar_is_visible(FormatBar* bar);
