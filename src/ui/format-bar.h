#pragma once

#include "editor-panel.h"

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
 * document open, therefore leaves the button where it was.
 *
 * The block styles are a dropdown rather than more buttons, because they are
 * one answer and not a set: a line is a heading *instead of* a paragraph, where
 * it can be bold *as well as* italic. A row of toggles could only ever have one
 * lit, which is a dropdown drawn the long way round.
 *
 * The two **list** styles are the exception, and get buttons of their own after
 * the inline ones. A list is the block kind an author turns on and off rather
 * than picks — a paragraph becomes a list and stops being one, where it does
 * not stop being a heading but becomes something else instead — and on-and-off
 * is what a toggle draws. Both controls show the same line: standing in a
 * bulleted list lights the button *and* reads "Bulleted List" in the dropdown.
 *
 * Everything follows the text by the one rule, through
 * format_bar_show_block(), which moves the dropdown and both buttons together.
 * Taking a list back off is the verb's own doing, not this bar's — see
 * editor_panel_set_block_style(). */
typedef struct FormatBar FormatBar;

FormatBar* format_bar_new(void);
void       format_bar_free(FormatBar* bar);

/* Borrowed. Owned by the widget hierarchy once parented. */
GtkWidget* format_bar_widget(FormatBar* bar);

/** Light the buttons for the styles in `flags`, a set of
 *  WORDSMITH_MARKUP_SPAN_* bits, and put out the rest. This is the only thing
 *  that moves a button: see the note above. */
void format_bar_show_styles(FormatBar* bar, uint32_t flags);

/** Show `style` as the block the cursor stands in. EDITOR_BLOCK_OTHER — a
 *  selection spanning two kinds, a code block, nothing open — leaves the
 *  dropdown showing nothing rather than picking one of the answers it could
 *  not give. */
void format_bar_show_block(FormatBar* bar, EditorBlockStyle style);

/** Put the bar on screen, or away. Hiding it leaves the editor the height it
 *  gives up. */
void     format_bar_set_visible(FormatBar* bar, gboolean visible);
gboolean format_bar_is_visible(FormatBar* bar);
