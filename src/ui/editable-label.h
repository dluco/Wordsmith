#pragma once

#include <gtk/gtk.h>

/* A name that can be typed over: a label until an edit starts, an entry while
 * one is in progress, and the same text in the same place either way.
 *
 * GtkEditableLabel is this widget, and is not used for two reasons. It offers
 * no way to ellipsize the label it shows, so one long chapter name would make
 * the whole binder scroll sideways; and it starts editing on a double click,
 * which is not the gesture wanted here — a slow second click on a row that is
 * already selected is. Neither is reachable from outside the widget.
 *
 * The two faces are a GtkStack of a GtkLabel and a **GtkText** — the bare
 * text-editing widget, not a GtkEntry. That is the whole of why the name stays
 * put when an edit opens over it: GtkEntry is a frame, ~8px of padding, a
 * minimum height and a focus ring around a GtkText, so swapping one in shoves
 * the name sideways and draws a box the size of the row around it. A `text`
 * node on its own has none of that.
 *
 * GtkEditableLabel is built the same way, and this widget deliberately does not
 * wear its CSS name, which those same nodes would otherwise match. A theme
 * paints that widget the way it paints a field: the view background, and text
 * in the view's foreground colour. In a row of a list that is a darker slab cut
 * out of the sidebar, and on a selected row it fights the selection it is drawn
 * inside. Under a name of its own the `text` node matches nothing a theme knows
 * about, and inherits the row's colour like any other label. The stylesheet's
 * `.binder-name` rules are then the whole of the look: padding on both faces
 * equally, so the two agree on where the text begins, and a selection the
 * author can see against whatever the row is wearing.
 *
 * The stack is homogeneous, so the row is the height of the taller face
 * whichever is showing and nothing moves vertically either.
 *
 * This is not a GtkEditable. The delegate boilerplate would buy an interface
 * nothing here calls; the two accessors below are the whole of what a row
 * needs.
 *
 * The widget owns the three ways out of an edit — Enter and clicking away keep
 * what was typed, Escape throws it away — and reports each of them through
 * ::editing-done. It does not own the way *in*: an edit starts when it is asked
 * for, so the gesture that asks is the caller's to choose. */

#define WORDSMITH_TYPE_EDITABLE_LABEL (wordsmith_editable_label_get_type())
G_DECLARE_FINAL_TYPE(WordsmithEditableLabel, wordsmith_editable_label, WORDSMITH,
                     EDITABLE_LABEL, GtkWidget)

GtkWidget* wordsmith_editable_label_new(void);

/** The name being shown. While an edit is in progress this is still the name
 *  the widget had when it started: what is being typed does not become the
 *  text until it is committed. Borrowed, and only until the next change. */
const char* wordsmith_editable_label_get_text(WordsmithEditableLabel* self);

void wordsmith_editable_label_set_text(WordsmithEditableLabel* self,
                                       const char* text);

gboolean wordsmith_editable_label_get_editing(WordsmithEditableLabel* self);

/** Open the entry over the name and put the cursor in it, selected whole — the
 *  way every rename in a file manager starts: the common case is replacing the
 *  name, and an author who wanted to amend it only has to press an arrow key
 *  first. Does nothing if an edit is already in progress. */
void wordsmith_editable_label_start_editing(WordsmithEditableLabel* self);

/** End the edit in progress, keeping what was typed or throwing it away, and
 *  emit ::editing-done. Committing makes the typed text the widget's text,
 *  including when it is empty — a widget showing names has no business deciding
 *  that an empty one means something, so whoever cares puts the old name back.
 *
 *  Does nothing if no edit is in progress, which is what makes it safe to call
 *  from a handler that may itself have been reached by ending one. */
void wordsmith_editable_label_stop_editing(WordsmithEditableLabel* self,
                                           gboolean commit);
