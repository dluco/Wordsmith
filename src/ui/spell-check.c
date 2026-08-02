#include "spell-check.h"

#include "core/preferences-c.h"
#include "core/spelling-c.h"

/* The answer in force, read once at startup and kept here so the menu's check
 * mark and the marks on the page cannot drift apart. */
static gboolean spelling_wanted = WORDSMITH_SPELL_CHECK_DEFAULT != 0;

/* How many of the dictionary's suggestions are offered. It will happily give
 * twenty variations on one typo; the list is there to be read at a glance. */
#define MAX_CORRECTIONS 8

struct SpellCheck {
    GtkTextBuffer* buffer;       /* borrowed */
    GtkTextTag*    misspelled;   /* owned by the buffer's tag table */

    /* Opened on the first word checked rather than here, so turning the marking
     * off costs nothing at all. One per buffer: sharing one across windows
     * would save a few megabytes and buy a question about who frees it, and
     * there is one window. */
    WordsmithSpellChecker* checker;
    gboolean               checker_tried;

    GPtrArray* skipped;   /* GtkTextTag*, borrowed */

    gboolean enabled;
    int      held;          /* nesting depth of hold/release */
    int      cursor_line;   /* where the cursor was, so leaving rechecks it */

    /* Where the last insertion or deletion left the text, or -1 when nothing
     * has been edited. The cursor standing here is what makes a word one in
     * progress rather than one being looked at; see
     * spell_check_word_being_typed(). */
    int typed_at;

    /* The view the corrections are offered in, and the word the last secondary
     * click landed on. The view is held with a reference because the gesture on
     * it points back here and has to come off before this is freed, which may
     * be after the window has taken the widget apart. The menu is handed to the
     * view once and filled and emptied from then on; the marks are the target
     * rather than two offsets, so an edit between the menu being filled and an
     * item being chosen cannot put the correction somewhere else. */
    GtkTextView*        view;
    GMenu*              menu;      /* owned */
    GSimpleActionGroup* actions;
    GtkTextMark*        word_start;
    GtkTextMark*        word_end;
    char*               word;
    GtkEventController* secondary_click;   /* borrowed; owned by the view */
    gboolean            choosing;   /* an item of the menu is being acted on */
};

/* ── the answer ──────────────────────────────────────────────────────────── */

void spell_check_init(void)
{
    spelling_wanted = wordsmith_preferences_spell_check() != 0;
}

gboolean spell_check_wanted(void)
{
    return spelling_wanted;
}

int spell_check_set_wanted(gboolean wanted, char** error)
{
    spelling_wanted = wanted;
    return wordsmith_preferences_set_spell_check(wanted ? 1 : 0, error);
}

/* ── the marking ─────────────────────────────────────────────────────────── */

gboolean spell_check_word_being_typed(int cursor, int typed_at, int from, int to)
{
    return cursor == typed_at && cursor > from && cursor <= to;
}

static WordsmithSpellChecker* checker_for(SpellCheck* spelling)
{
    if (!spelling->checker_tried) {
        spelling->checker_tried = TRUE;
        spelling->checker       = wordsmith_spell_checker_new();
    }
    return spelling->checker;
}

gboolean spell_check_available(SpellCheck* spelling)
{
    return spelling != NULL
        && wordsmith_spell_checker_available(checker_for(spelling)) != 0;
}

static gboolean word_is_skipped(SpellCheck* spelling, const GtkTextIter* start)
{
    for (guint index = 0; index < spelling->skipped->len; index++) {
        if (gtk_text_iter_has_tag(start, g_ptr_array_index(spelling->skipped, index))) {
            return TRUE;
        }
    }
    return FALSE;
}

static void clear_marks(SpellCheck* spelling, const GtkTextIter* start,
                        const GtkTextIter* end)
{
    gtk_text_buffer_remove_tag(spelling->buffer, spelling->misspelled, start, end);
}

/* One line: take the marks off, then put back the ones the dictionary still
 * asks for. Rechecking rather than patching is what makes every edit — typing,
 * pasting, an undo putting a paragraph back — arrive at the same answer. */
static void recheck_line(SpellCheck* spelling, int line)
{
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_line(spelling->buffer, &start, line);

    GtkTextIter end = start;
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }
    clear_marks(spelling, &start, &end);

    char* text = gtk_text_buffer_get_text(spelling->buffer, &start, &end, FALSE);
    if (text == NULL || text[0] == '\0') {
        g_free(text);
        return;
    }

    /* The dictionary is asked what holds a word together in its own language
     * before it is asked about any word, so a line is split the way the language
     * splits it. Nothing is opened that would not have been: this is a line with
     * text on it, which is a line that was going to be checked. */
    WordsmithSpellChecker* checker = checker_for(spelling);

    size_t              count = 0;
    WordsmithSpellWord* words = wordsmith_spell_words(
        text, wordsmith_spell_checker_extra_word_chars(checker), &count);
    g_free(text);
    if (words == NULL) {
        return;
    }

    const int line_offset = gtk_text_iter_get_offset(&start);

    GtkTextIter at_cursor;
    gtk_text_buffer_get_iter_at_mark(spelling->buffer, &at_cursor,
                                     gtk_text_buffer_get_insert(spelling->buffer));
    const int cursor = gtk_text_iter_get_offset(&at_cursor);

    for (size_t index = 0; index < count; index++) {
        const int from = line_offset + words[index].offset;
        const int to   = from + words[index].length;

        if (spell_check_word_being_typed(cursor, spelling->typed_at, from, to)) {
            continue;
        }

        GtkTextIter word_start;
        GtkTextIter word_end;
        gtk_text_buffer_get_iter_at_offset(spelling->buffer, &word_start, from);
        gtk_text_buffer_get_iter_at_offset(spelling->buffer, &word_end, to);

        if (word_is_skipped(spelling, &word_start)) {
            continue;
        }
        if (wordsmith_spell_checker_knows(checker, words[index].text) != 0) {
            continue;
        }

        gtk_text_buffer_apply_tag(spelling->buffer, spelling->misspelled,
                                  &word_start, &word_end);
    }

    wordsmith_spell_words_free(words, count);
}

static void recheck_lines(SpellCheck* spelling, int first, int last)
{
    if (!spelling->enabled || spelling->held > 0) {
        return;
    }

    const int line_count = gtk_text_buffer_get_line_count(spelling->buffer);
    if (first < 0) {
        first = 0;
    }
    if (last >= line_count) {
        last = line_count - 1;
    }

    for (int line = first; line <= last; line++) {
        recheck_line(spelling, line);
    }
}

void spell_check_refresh(SpellCheck* spelling)
{
    if (spelling == NULL) {
        return;
    }
    recheck_lines(spelling, 0, gtk_text_buffer_get_line_count(spelling->buffer) - 1);
}

/* ── following the buffer ────────────────────────────────────────────────── */

/* Defined with the menu, and needed here: the cursor moving is one of the ways
 * the offer over a word stops being about anything. */
static void withdraw_offer(SpellCheck* spelling);

/* After the text has landed, so the line it landed on can be read. The
 * insertion may have carried newlines — a paste of three paragraphs — so the
 * span is worked out from the text's own length rather than assumed to be one
 * line. */
static void on_text_inserted(GtkTextBuffer* buffer, GtkTextIter* location,
                             char* text, int length, gpointer user_data)
{
    (void) buffer;

    SpellCheck* spelling = user_data;
    const int   last     = gtk_text_iter_get_line(location);

    /* Before the recheck, since it is what the recheck reads: the text has just
     * been written, so the word it ends in is the one in progress. */
    spelling->typed_at = gtk_text_iter_get_offset(location);

    int first = last;
    for (const char* at = text; at != NULL && at < text + length; at++) {
        if (*at == '\n') {
            first--;
        }
    }

    recheck_lines(spelling, first, last);
    spelling->cursor_line = last;
}

/* After the text has gone, when both ends of the range have collapsed onto the
 * one line left behind. */
static void on_range_deleted(GtkTextBuffer* buffer, GtkTextIter* start,
                             GtkTextIter* end, gpointer user_data)
{
    (void) buffer;
    (void) end;

    SpellCheck* spelling = user_data;
    const int   line     = gtk_text_iter_get_line(start);

    /* Both ends of the range have collapsed onto the same place, which is where
     * a backspace leaves the cursor and so where a word may still be under the
     * author's hands. */
    spelling->typed_at = gtk_text_iter_get_offset(start);

    recheck_lines(spelling, line, line);
    spelling->cursor_line = line;
}

/* The word being typed is not marked, so the line the cursor leaves has to be
 * looked at again — that is where the word it was standing in has been left
 * unmarked and finished. The line it arrives on is rechecked too, to take the
 * mark off the word it has just stepped into. */
static void on_cursor_moved(GObject* buffer, GParamSpec* spec, gpointer user_data)
{
    (void) spec;

    SpellCheck* spelling = user_data;

    GtkTextIter at;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(buffer), &at,
                                     gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(buffer)));
    const int line = gtk_text_iter_get_line(&at);
    const int left = spelling->cursor_line;
    spelling->cursor_line = line;

    /* The offer belongs to the word the last click was on. The cursor going
     * anywhere is the author's attention going with it, and GtkTextView opens
     * this menu from the keyboard as well — where nothing gets the chance to
     * prepare it, so what is left over is all it would show. */
    if (!spelling->choosing) {
        withdraw_offer(spelling);
    }

    if (left != line) {
        recheck_lines(spelling, left, left);
    }
    recheck_lines(spelling, line, line);
}

/* ── the menu over a misspelling ─────────────────────────────────────────── */

void spell_check_fill_menu(GMenu* menu, const char* word,
                           const char* const* corrections, size_t count)
{
    g_menu_remove_all(menu);

    if (count > MAX_CORRECTIONS) {
        count = MAX_CORRECTIONS;
    }

    GMenu* offered = g_menu_new();
    for (size_t index = 0; index < count; index++) {
        GMenuItem* item = g_menu_item_new(corrections[index], NULL);
        /* The replacement travels as the item's target rather than as an index
         * into a list this file would then have to keep: the menu says what it
         * will do, and the action needs nothing but the word and the marks. */
        g_menu_item_set_action_and_target_value(
            item, "spelling.correct", g_variant_new_string(corrections[index]));
        g_menu_append_item(offered, item);
        g_object_unref(item);
    }
    if (count == 0) {
        /* An action that exists and is switched off, so the item is there and
         * greyed rather than absent — the same reason the Edit menu greys an
         * undo with nothing behind it instead of dropping it. */
        g_menu_append(offered, "No Suggestions", "spelling.no-suggestions");
    }

    /* The word as the section's heading: the items land under the view's own
     * cut and paste, where a bare list of near-words would be a puzzle. */
    g_menu_append_section(menu, word, G_MENU_MODEL(offered));
    g_object_unref(offered);

    GMenu* dictionary = g_menu_new();
    /* For this sitting, and for good: the first is for a name this manuscript
     * is full of, the second for a word the author means beyond it. */
    g_menu_append(dictionary, "Ignore All", "spelling.ignore");
    g_menu_append(dictionary, "Add to Dictionary", "spelling.add");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(dictionary));
    g_object_unref(dictionary);
}

/* Take the offer back: the menu empties and the word it was about is forgotten.
 * The model itself stays where it is — the view keeps the popover it built from
 * it and follows the change — because handing over another would throw that
 * popover away, and from an item's own handler that means pulling the menu out
 * from under the click that chose it. */
static void withdraw_offer(SpellCheck* spelling)
{
    if (spelling->menu != NULL) {
        g_menu_remove_all(spelling->menu);
    }
    if (spelling->word_start != NULL) {
        gtk_text_buffer_delete_mark(spelling->buffer, spelling->word_start);
        spelling->word_start = NULL;
    }
    if (spelling->word_end != NULL) {
        gtk_text_buffer_delete_mark(spelling->buffer, spelling->word_end);
        spelling->word_end = NULL;
    }
    g_clear_pointer(&spelling->word, g_free);
}

/* The marked word the menu is about, or FALSE when it has been edited away. */
static gboolean menu_target(SpellCheck* spelling, GtkTextIter* start, GtkTextIter* end)
{
    if (spelling->word_start == NULL || spelling->word_end == NULL) {
        return FALSE;
    }
    gtk_text_buffer_get_iter_at_mark(spelling->buffer, start, spelling->word_start);
    gtk_text_buffer_get_iter_at_mark(spelling->buffer, end, spelling->word_end);
    return gtk_text_iter_compare(start, end) < 0;
}

/* One word replaced by another, as a plain delete and insert. The editor's own
 * handlers are on both, so the correction is recorded, marks the document
 * modified and rechecks the line without this file arranging any of it. */
static void on_correct(GSimpleAction* action, GVariant* value, gpointer user_data)
{
    (void) action;

    SpellCheck* spelling = user_data;
    GtkTextIter start;
    GtkTextIter end;
    if (value == NULL || !menu_target(spelling, &start, &end)) {
        return;
    }

    const char* correction = g_variant_get_string(value, NULL);

    /* The edit below moves the cursor, and a cursor that moves takes the offer
     * back — which would empty this menu while the click that chose an item of
     * it is still being handled. */
    spelling->choosing = TRUE;
    gtk_text_buffer_begin_user_action(spelling->buffer);
    gtk_text_buffer_delete(spelling->buffer, &start, &end);
    gtk_text_buffer_insert(spelling->buffer, &start, correction, -1);
    gtk_text_buffer_end_user_action(spelling->buffer);
    spelling->choosing = FALSE;
}

static void on_ignore(GSimpleAction* action, GVariant* value, gpointer user_data)
{
    (void) action;
    (void) value;

    SpellCheck* spelling = user_data;
    if (spelling->word == NULL) {
        return;
    }
    wordsmith_spell_checker_accept(checker_for(spelling), spelling->word);
    /* Every other copy of the word loses its line too, which is what "all"
     * means and what an author who has just met their protagonist wants. */
    spell_check_refresh(spelling);
}

static void on_add(GSimpleAction* action, GVariant* value, gpointer user_data)
{
    (void) action;
    (void) value;

    SpellCheck* spelling = user_data;
    if (spelling->word == NULL) {
        return;
    }
    wordsmith_spell_checker_remember(checker_for(spelling), spelling->word);
    spell_check_refresh(spelling);
}

/* The misspelled word `at` stands in, if it is in one. The tag's own toggles
 * are the extent — it was put there over exactly one word — so nothing here
 * needs to find a word boundary for itself. */
static gboolean misspelling_at(SpellCheck* spelling, const GtkTextIter* at,
                               GtkTextIter* start, GtkTextIter* end)
{
    if (!gtk_text_iter_has_tag(at, spelling->misspelled)) {
        return FALSE;
    }

    *start = *at;
    *end   = *at;
    if (!gtk_text_iter_starts_tag(start, spelling->misspelled)) {
        gtk_text_iter_backward_to_tag_toggle(start, spelling->misspelled);
    }
    gtk_text_iter_forward_to_tag_toggle(end, spelling->misspelled);
    return gtk_text_iter_compare(start, end) < 0;
}

/* Ahead of GtkTextView's own gesture, which is what makes this the last chance
 * to say what the menu holds. The press is deliberately not claimed: the view
 * still opens its context menu, and this has only filled in the part of it that
 * is about the word underneath. */
static void on_secondary_press(GtkGestureClick* gesture, int n_press, double x,
                               double y, gpointer user_data)
{
    (void) gesture;
    (void) n_press;

    SpellCheck* spelling = user_data;

    withdraw_offer(spelling);
    if (!spelling->enabled) {
        return;
    }

    int buffer_x = 0;
    int buffer_y = 0;
    gtk_text_view_window_to_buffer_coords(spelling->view, GTK_TEXT_WINDOW_WIDGET,
                                          (int) x, (int) y, &buffer_x, &buffer_y);

    GtkTextIter at;
    if (!gtk_text_view_get_iter_at_location(spelling->view, &at, buffer_x, buffer_y)) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    if (!misspelling_at(spelling, &at, &start, &end)) {
        return;
    }

    spelling->word = gtk_text_buffer_get_text(spelling->buffer, &start, &end, FALSE);
    /* Gravity outwards, so the marks still hold the word after the delete that
     * a correction begins with. */
    spelling->word_start = gtk_text_buffer_create_mark(spelling->buffer, NULL,
                                                       &start, TRUE);
    spelling->word_end = gtk_text_buffer_create_mark(spelling->buffer, NULL, &end,
                                                     FALSE);

    size_t count       = 0;
    char** corrections = wordsmith_spell_checker_corrections(checker_for(spelling),
                                                             spelling->word, &count);

    spell_check_fill_menu(spelling->menu, spelling->word,
                          (const char* const*) corrections, count);
    wordsmith_spell_corrections_free(corrections, count);
}

void spell_check_attach_menu(SpellCheck* spelling, GtkTextView* view)
{
    if (spelling == NULL || view == NULL || spelling->view != NULL) {
        return;
    }
    spelling->view = g_object_ref(view);

    static const GActionEntry ENTRIES[] = {
        { "correct", on_correct, "s",  NULL, NULL, { 0 } },
        { "ignore",  on_ignore,  NULL, NULL, NULL, { 0 } },
        { "add",     on_add,     NULL, NULL, NULL, { 0 } },
    };

    spelling->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(spelling->actions), ENTRIES,
                                    G_N_ELEMENTS(ENTRIES), spelling);

    /* Never enabled: it exists so the item it names can be shown greyed. */
    GSimpleAction* nothing = g_simple_action_new("no-suggestions", NULL);
    g_simple_action_set_enabled(nothing, FALSE);
    g_action_map_add_action(G_ACTION_MAP(spelling->actions), G_ACTION(nothing));
    g_object_unref(nothing);

    /* On the view, so the popover the view parents finds the group by walking
     * up to it — the same way the binder's rows find the window's actions. */
    gtk_widget_insert_action_group(GTK_WIDGET(view), "spelling",
                                   G_ACTION_GROUP(spelling->actions));

    /* Handed over empty and once. From here on the offer is made and taken back
     * by filling and emptying it, which the view follows. */
    spelling->menu = g_menu_new();
    gtk_text_view_set_extra_menu(view, G_MENU_MODEL(spelling->menu));

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_secondary_press), spelling);
    gtk_widget_add_controller(GTK_WIDGET(view), GTK_EVENT_CONTROLLER(click));
    spelling->secondary_click = GTK_EVENT_CONTROLLER(click);
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

SpellCheck* spell_check_new(GtkTextBuffer* buffer)
{
    if (buffer == NULL) {
        return NULL;
    }

    SpellCheck* spelling = g_new0(SpellCheck, 1);
    spelling->buffer   = buffer;
    spelling->skipped  = g_ptr_array_new();
    spelling->enabled  = spell_check_wanted();
    spelling->typed_at = -1;   /* nothing has been typed anywhere yet */

    /* PANGO_UNDERLINE_ERROR is the wavy line every other editor draws. The
     * colour has to be said outright: left alone the line takes the colour of
     * the text, which is a grey squiggle in a dark theme and no warning at all.
     * It cannot come from the stylesheet either — a GtkTextTag is not a widget
     * and CSS cannot reach one — so this is the one piece of the look that
     * lives in C rather than in style.css. */
    GdkRGBA red;
    gdk_rgba_parse(&red, "#e01b24");
    spelling->misspelled = gtk_text_buffer_create_tag(
        buffer, "misspelled",
        "underline", PANGO_UNDERLINE_ERROR,
        "underline-rgba", &red,
        NULL);

    g_signal_connect_after(buffer, "insert-text", G_CALLBACK(on_text_inserted),
                           spelling);
    g_signal_connect_after(buffer, "delete-range", G_CALLBACK(on_range_deleted),
                           spelling);
    g_signal_connect(buffer, "notify::cursor-position", G_CALLBACK(on_cursor_moved),
                     spelling);

    return spelling;
}

void spell_check_free(SpellCheck* spelling)
{
    if (spelling == NULL) {
        return;
    }

    /* The buffer may well outlive this — the editor panel is freed with its
     * window, and a GtkTextBuffer belongs to its view — so the handlers have to
     * come off, or the next edit would reach into freed memory. */
    g_signal_handlers_disconnect_by_data(spelling->buffer, spelling);
    withdraw_offer(spelling);

    /* And the same for the view: the gesture, the actions and the menu all
     * point back here. The reference taken in spell_check_attach_menu() is what
     * makes the widget still be there to take them off. */
    if (spelling->view != NULL) {
        if (spelling->secondary_click != NULL) {
            gtk_widget_remove_controller(GTK_WIDGET(spelling->view),
                                         spelling->secondary_click);
        }
        gtk_widget_insert_action_group(GTK_WIDGET(spelling->view), "spelling", NULL);
        gtk_text_view_set_extra_menu(spelling->view, NULL);
        g_clear_object(&spelling->view);
    }
    g_clear_object(&spelling->menu);
    g_clear_object(&spelling->actions);

    wordsmith_spell_checker_free(spelling->checker);
    g_ptr_array_free(spelling->skipped, TRUE);
    g_free(spelling);
}

void spell_check_skip_tag(SpellCheck* spelling, GtkTextTag* tag)
{
    if (spelling != NULL && tag != NULL) {
        g_ptr_array_add(spelling->skipped, tag);
    }
}

void spell_check_set_enabled(SpellCheck* spelling, gboolean enabled)
{
    if (spelling == NULL || spelling->enabled == enabled) {
        return;
    }
    spelling->enabled = enabled;

    if (enabled) {
        spell_check_refresh(spelling);
        return;
    }

    /* Nothing is marked any more, so an offer about a word that no longer looks
     * wrong goes with the marks. */
    withdraw_offer(spelling);

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(spelling->buffer, &start, &end);
    clear_marks(spelling, &start, &end);
}

void spell_check_hold(SpellCheck* spelling)
{
    if (spelling != NULL) {
        spelling->held++;
    }
}

void spell_check_release(SpellCheck* spelling)
{
    if (spelling == NULL || spelling->held == 0) {
        return;
    }
    if (--spelling->held == 0) {
        spelling->cursor_line = 0;
        /* A document arriving is not a word in progress, whatever the last
         * insertion of the load happened to leave behind. */
        spelling->typed_at = -1;
        spell_check_refresh(spelling);
    }
}
