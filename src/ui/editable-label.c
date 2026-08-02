#include "editable-label.h"

#define PAGE_DISPLAY "display"
#define PAGE_EDIT    "edit"

/* Carried while an edit is in progress, the way GtkEditableLabel carries it, so
 * a stylesheet can tell the two states apart without asking the widget. */
#define EDITING_CLASS "editing"

struct _WordsmithEditableLabel {
    GtkWidget parent_instance;

    GtkWidget* stack;
    GtkWidget* label;
    GtkWidget* text;

    gboolean editing;
};

G_DEFINE_FINAL_TYPE(WordsmithEditableLabel, wordsmith_editable_label, GTK_TYPE_WIDGET)

enum { EDITING_DONE, N_SIGNALS };

static guint signals[N_SIGNALS];

/* ── ending an edit ──────────────────────────────────────────────────────── */

void wordsmith_editable_label_stop_editing(WordsmithEditableLabel* self,
                                           gboolean commit)
{
    g_return_if_fail(WORDSMITH_IS_EDITABLE_LABEL(self));

    if (!self->editing) {
        return;
    }

    /* Cleared first, before anything that could fire another signal: putting the
     * label back takes the focus off the entry, and the focus handler lands here
     * too. Clearing first is what makes the second call a no-op rather than a
     * second answer. */
    self->editing = FALSE;

    if (commit) {
        gtk_label_set_text(GTK_LABEL(self->label),
                           gtk_editable_get_text(GTK_EDITABLE(self->text)));
    }
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), PAGE_DISPLAY);
    gtk_widget_remove_css_class(GTK_WIDGET(self), EDITING_CLASS);

    g_signal_emit(self, signals[EDITING_DONE], 0, commit);
}

static void on_text_activate(GtkText* text, gpointer user_data)
{
    (void) text;
    wordsmith_editable_label_stop_editing(user_data, TRUE);
}

static gboolean on_text_key(GtkEventControllerKey* controller, guint keyval,
                            guint keycode, GdkModifierType modifiers,
                            gpointer user_data)
{
    (void) controller;
    (void) keycode;
    (void) modifiers;

    if (keyval != GDK_KEY_Escape) {
        return GDK_EVENT_PROPAGATE;
    }
    wordsmith_editable_label_stop_editing(user_data, FALSE);
    return GDK_EVENT_STOP;
}

/* Clicking away keeps what was typed, which is what every list that renames in
 * place does, and what someone who has stopped looking at the entry means. */
static void on_text_focus_leave(GtkEventControllerFocus* controller,
                                gpointer user_data)
{
    (void) controller;
    wordsmith_editable_label_stop_editing(user_data, TRUE);
}

/* ── the rest of the surface ─────────────────────────────────────────────── */

void wordsmith_editable_label_start_editing(WordsmithEditableLabel* self)
{
    g_return_if_fail(WORDSMITH_IS_EDITABLE_LABEL(self));

    if (self->editing) {
        return;
    }
    self->editing = TRUE;

    gtk_editable_set_text(GTK_EDITABLE(self->text),
                          gtk_label_get_text(GTK_LABEL(self->label)));
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), PAGE_EDIT);
    gtk_widget_add_css_class(GTK_WIDGET(self), EDITING_CLASS);

    /* Selected after the focus rather than before it: taking the focus is what
     * places the cursor, and it would undo a selection made ahead of it. */
    gtk_widget_grab_focus(self->text);
    gtk_editable_select_region(GTK_EDITABLE(self->text), 0, -1);
}

gboolean wordsmith_editable_label_get_editing(WordsmithEditableLabel* self)
{
    g_return_val_if_fail(WORDSMITH_IS_EDITABLE_LABEL(self), FALSE);
    return self->editing;
}

const char* wordsmith_editable_label_get_text(WordsmithEditableLabel* self)
{
    g_return_val_if_fail(WORDSMITH_IS_EDITABLE_LABEL(self), "");
    return gtk_label_get_text(GTK_LABEL(self->label));
}

void wordsmith_editable_label_set_text(WordsmithEditableLabel* self, const char* text)
{
    g_return_if_fail(WORDSMITH_IS_EDITABLE_LABEL(self));

    gtk_label_set_text(GTK_LABEL(self->label), text != NULL ? text : "");
}

GtkWidget* wordsmith_editable_label_new(void)
{
    return g_object_new(WORDSMITH_TYPE_EDITABLE_LABEL, NULL);
}

/* ── type ────────────────────────────────────────────────────────────────── */

static void wordsmith_editable_label_dispose(GObject* object)
{
    WordsmithEditableLabel* self = WORDSMITH_EDITABLE_LABEL(object);

    /* A widget torn down mid-edit is an edit ending, and whoever started it is
     * holding a pointer to this one. Thrown away rather than kept — the row is
     * going away, so this is not anybody's answer to the entry — and reported
     * while the children are still here to report it with, so nothing outside
     * is left pointing at a widget that has gone. */
    wordsmith_editable_label_stop_editing(self, FALSE);

    g_clear_pointer(&self->stack, gtk_widget_unparent);
    self->label = NULL;
    self->text  = NULL;

    G_OBJECT_CLASS(wordsmith_editable_label_parent_class)->dispose(object);
}

static void wordsmith_editable_label_class_init(WordsmithEditableLabelClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = wordsmith_editable_label_dispose;

    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
    /* Its own name rather than GtkEditableLabel's, which the nodes under it
     * would otherwise match: see the header. */
    gtk_widget_class_set_css_name(widget_class, "editable-label");

    /* `commit` says which of the ways out was taken: TRUE for Enter and for
     * clicking away, FALSE for Escape and for the widget being torn down. The
     * text has already been kept or thrown away by the time this arrives — it
     * reports the end of an edit rather than asking what to do about it. */
    signals[EDITING_DONE] =
        g_signal_new("editing-done", WORDSMITH_TYPE_EDITABLE_LABEL, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void wordsmith_editable_label_init(WordsmithEditableLabel* self)
{
    self->stack = gtk_stack_new();
    self->label = gtk_label_new(NULL);
    self->text  = gtk_text_new();

    gtk_label_set_xalign(GTK_LABEL(self->label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->label), PANGO_ELLIPSIZE_END);

    /* The entry asks for no width of its own, so opening it over a name does not
     * shove the row wider than the pane it is in. */
    gtk_editable_set_width_chars(GTK_EDITABLE(self->text), 0);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->text), 0);

    gtk_stack_add_named(GTK_STACK(self->stack), self->label, PAGE_DISPLAY);
    gtk_stack_add_named(GTK_STACK(self->stack), self->text, PAGE_EDIT);
    gtk_widget_set_parent(self->stack, GTK_WIDGET(self));

    g_signal_connect(self->text, "activate", G_CALLBACK(on_text_activate), self);

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_text_key), self);
    gtk_widget_add_controller(self->text, keys);

    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_text_focus_leave), self);
    gtk_widget_add_controller(self->text, focus);
}
