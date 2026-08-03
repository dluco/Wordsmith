#include "format-bar.h"

#include "core/markup-c.h"

/* One button per inline style the editor can carry and the Format menu can
 * reach. The accelerator is spelled into the tooltip because the bar is the
 * one place a shortcut can be discovered without opening a menu. */
typedef struct ButtonSpec {
    const char* icon;
    const char* action;    /* the window action the button raises */
    const char* tooltip;
    uint32_t    span_flag; /* the style whose presence lights it up */
} ButtonSpec;

static const ButtonSpec BUTTONS[] = {
    { "format-text-bold-symbolic", "win.format-bold", "Bold (Ctrl+B)",
      WORDSMITH_MARKUP_SPAN_STRONG },
    { "format-text-italic-symbolic", "win.format-italic", "Italic (Ctrl+I)",
      WORDSMITH_MARKUP_SPAN_EMPHASIS },
    { "format-text-underline-symbolic", "win.format-underline",
      "Underline (Ctrl+U)", WORDSMITH_MARKUP_SPAN_UNDERLINE },
};

#define BUTTON_COUNT G_N_ELEMENTS(BUTTONS)

/* The two list styles are block styles, and they still get buttons.
 *
 * A list is the one block kind an author turns *on and off* rather than picks:
 * a paragraph becomes a list and stops being one, where it does not become a
 * heading and stop being one — it becomes something else instead. That is a
 * toggle, and a toggle is a button. The other five stay in the dropdown, where
 * an exclusive set of kinds belongs, and both controls show the same line
 * honestly: standing in a bulleted list lights this button *and* reads
 * "Bulleted List" in the dropdown.
 *
 * They name win.block-style with a target rather than an action of their own,
 * so the button, the Format menu and Ctrl+Shift+8 stay three ways into one
 * verb — including the way back out, which is the verb's own doing and not
 * something this bar knows about. */
typedef struct ListButtonSpec {
    const char*      icon;
    const char*      tooltip;
    EditorBlockStyle style;
} ListButtonSpec;

static const ListButtonSpec LIST_BUTTONS[] = {
    { "view-list-bullet-symbolic", "Bulleted List (Ctrl+Shift+8)",
      EDITOR_BLOCK_BULLET_LIST },
    { "view-list-ordered-symbolic", "Numbered List (Ctrl+Shift+7)",
      EDITOR_BLOCK_NUMBERED_LIST },
};

#define LIST_BUTTON_COUNT G_N_ELEMENTS(LIST_BUTTONS)

struct FormatBar {
    GtkWidget* root; /* borrowed once parented into the window */
    GtkWidget* buttons[BUTTON_COUNT];
    GtkWidget* list_buttons[LIST_BUTTON_COUNT];
    GtkWidget* blocks;   /* the block style dropdown */

    /* Set while the bar is following the text. A GtkToggleButton reports every
     * change to its state, including the ones made from here, and a
     * GtkDropDown reports every change to its selection the same way; acting
     * on those would raise the action that the report was the answer to. */
    gboolean updating;
};

static void on_button_toggled(GtkToggleButton* button, gpointer user_data)
{
    FormatBar* bar = user_data;
    if (bar->updating) {
        return;
    }

    for (gsize index = 0; index < BUTTON_COUNT; index++) {
        if (bar->buttons[index] == GTK_WIDGET(button)) {
            gtk_widget_activate_action(GTK_WIDGET(button), BUTTONS[index].action,
                                       NULL);
            return;
        }
    }
}

static void on_list_button_toggled(GtkToggleButton* button, gpointer user_data)
{
    FormatBar* bar = user_data;
    if (bar->updating) {
        return;
    }

    for (gsize index = 0; index < LIST_BUTTON_COUNT; index++) {
        if (bar->list_buttons[index] == GTK_WIDGET(button)) {
            gtk_widget_activate_action(GTK_WIDGET(button), "win.block-style", "s",
                                       editor_block_style_id(
                                           LIST_BUTTONS[index].style));
            return;
        }
    }
}

/* The dropdown's rows are EDITOR_BLOCK_* in order, so the selected position is
 * the style — no lookup table, and nothing to fall out of step. */
static void on_block_selected(GObject* dropdown, GParamSpec* spec,
                              gpointer user_data)
{
    (void) spec;

    FormatBar* bar = user_data;
    if (bar->updating) {
        return;
    }

    const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (selected == GTK_INVALID_LIST_POSITION
        || selected >= EDITOR_BLOCK_STYLE_COUNT) {
        return;
    }

    const char* id = editor_block_style_id((EditorBlockStyle) selected);
    gtk_widget_activate_action(GTK_WIDGET(dropdown), "win.block-style", "s", id);
}

static GtkWidget* build_block_dropdown(FormatBar* bar)
{
    const char* labels[EDITOR_BLOCK_STYLE_COUNT + 1];
    for (int index = 0; index < EDITOR_BLOCK_STYLE_COUNT; index++) {
        labels[index] = editor_block_style_name((EditorBlockStyle) index);
    }
    labels[EDITOR_BLOCK_STYLE_COUNT] = NULL;

    GtkWidget* dropdown = gtk_drop_down_new_from_strings(labels);
    gtk_widget_set_tooltip_text(dropdown, "Paragraph style");
    gtk_widget_add_css_class(dropdown, "flat");
    /* The manuscript keeps the keyboard, the same as the buttons: picking a
     * style formats the line and gives the caret straight back. */
    gtk_widget_set_focus_on_click(dropdown, FALSE);

    /* Nothing selected until the editor says so. An empty dropdown above an
     * empty pane is honest; "Paragraph" above no document would not be. */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), GTK_INVALID_LIST_POSITION);

    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_block_selected),
                     bar);
    return dropdown;
}

/* Every toggle on the bar is built the same way; only what it raises differs. */
static GtkWidget* add_toggle(FormatBar* bar, GtkWidget* box, const char* icon,
                             const char* tooltip, GCallback on_toggled)
{
    GtkWidget* button = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(button), icon);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_add_css_class(button, "flat");
    /* The manuscript keeps the keyboard: a click formats the selection and
     * gives it straight back, rather than leaving the caret behind. */
    gtk_widget_set_focus_on_click(button, FALSE);

    g_signal_connect(button, "toggled", on_toggled, bar);
    gtk_box_append(GTK_BOX(box), button);
    return button;
}

FormatBar* format_bar_new(void)
{
    FormatBar* bar = g_new0(FormatBar, 1);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(box, "toolbar");
    gtk_widget_add_css_class(box, "format-bar");

    bar->blocks = build_block_dropdown(bar);
    gtk_box_append(GTK_BOX(box), bar->blocks);

    GtkWidget* divider = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(box), divider);

    for (gsize index = 0; index < BUTTON_COUNT; index++) {
        bar->buttons[index] = add_toggle(bar, box, BUTTONS[index].icon,
                                         BUTTONS[index].tooltip,
                                         G_CALLBACK(on_button_toggled));
    }

    /* The list buttons are their own group: they are block styles, and putting
     * them straight after B, I and U would read as a fourth thing a character
     * can wear. */
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    for (gsize index = 0; index < LIST_BUTTON_COUNT; index++) {
        bar->list_buttons[index] = add_toggle(bar, box, LIST_BUTTONS[index].icon,
                                              LIST_BUTTONS[index].tooltip,
                                              G_CALLBACK(on_list_button_toggled));
    }

    bar->root = box;
    return bar;
}

void format_bar_free(FormatBar* bar)
{
    if (bar == NULL) {
        return;
    }
    g_free(bar);
}

GtkWidget* format_bar_widget(FormatBar* bar)
{
    return bar != NULL ? bar->root : NULL;
}

void format_bar_show_styles(FormatBar* bar, uint32_t flags)
{
    if (bar == NULL) {
        return;
    }

    bar->updating = TRUE;
    for (gsize index = 0; index < BUTTON_COUNT; index++) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bar->buttons[index]),
                                     (flags & BUTTONS[index].span_flag) != 0);
    }
    bar->updating = FALSE;
}

void format_bar_show_block(FormatBar* bar, EditorBlockStyle style)
{
    if (bar == NULL) {
        return;
    }

    const guint position = style >= 0 && style < EDITOR_BLOCK_STYLE_COUNT
                               ? (guint) style
                               : GTK_INVALID_LIST_POSITION;

    bar->updating = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(bar->blocks), position);
    /* Lit while the cursor stands in that kind of list, out the moment it
     * leaves — the same rule the inline buttons follow, and the same single
     * report from the editor moving both. */
    for (gsize index = 0; index < LIST_BUTTON_COUNT; index++) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bar->list_buttons[index]),
                                     style == LIST_BUTTONS[index].style);
    }
    bar->updating = FALSE;
}

void format_bar_set_visible(FormatBar* bar, gboolean visible)
{
    if (bar == NULL) {
        return;
    }
    gtk_widget_set_visible(bar->root, visible);
}

gboolean format_bar_is_visible(FormatBar* bar)
{
    return bar != NULL && gtk_widget_get_visible(bar->root);
}
