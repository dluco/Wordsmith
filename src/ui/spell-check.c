#include "spell-check.h"

#include "core/preferences-c.h"
#include "core/spelling-c.h"

/* The answer in force, read once at startup and kept here so the menu's check
 * mark and the marks on the page cannot drift apart. */
static gboolean spelling_wanted = WORDSMITH_SPELL_CHECK_DEFAULT != 0;

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

gboolean spell_check_cursor_in_word(int cursor, int from, int to)
{
    return cursor > from && cursor <= to;
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

    size_t              count = 0;
    WordsmithSpellWord* words = wordsmith_spell_words(text, &count);
    g_free(text);
    if (words == NULL) {
        return;
    }

    WordsmithSpellChecker* checker = checker_for(spelling);
    const int line_offset = gtk_text_iter_get_offset(&start);

    GtkTextIter at_cursor;
    gtk_text_buffer_get_iter_at_mark(spelling->buffer, &at_cursor,
                                     gtk_text_buffer_get_insert(spelling->buffer));
    const int cursor = gtk_text_iter_get_offset(&at_cursor);

    for (size_t index = 0; index < count; index++) {
        const int from = line_offset + words[index].offset;
        const int to   = from + words[index].length;

        if (spell_check_cursor_in_word(cursor, from, to)) {
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

    if (left != line) {
        recheck_lines(spelling, left, left);
    }
    recheck_lines(spelling, line, line);
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

SpellCheck* spell_check_new(GtkTextBuffer* buffer)
{
    if (buffer == NULL) {
        return NULL;
    }

    SpellCheck* spelling = g_new0(SpellCheck, 1);
    spelling->buffer  = buffer;
    spelling->skipped = g_ptr_array_new();
    spelling->enabled = spell_check_wanted();

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
        spell_check_refresh(spelling);
    }
}
