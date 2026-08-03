#include "editor-panel.h"

#include "spell-check.h"

#include "core/frontmatter-c.h"
#include "core/markup-c.h"
#include "core/preferences-c.h"
#include "core/project-c.h"

#include <string.h>

/* Inline style tags, indexed by bit position in WordsmithMarkupSpanFlags. */
#define INLINE_TAG_COUNT 4

/* Deepest ATX heading Markdown allows. */
#define MAX_HEADING_LEVEL 6

struct EditorPanel {
    GtkWidget*     root;   /* borrowed once parented into the window */
    GtkTextView*   view;
    GtkTextBuffer* buffer;

    char*    path;      /* owned; NULL when nothing is open */
    gboolean loading;   /* suppresses the modified callback during load */

    /* Everything before the Markdown body: a BOM, and the frontmatter with its
     * fences. Owned, "" when there is none. The buffer never holds these bytes
     * — the inspector owns that metadata — so they are kept verbatim here and
     * put back on save. Round-tripping a file whose YAML we only half
     * understood therefore costs the author nothing. */
    char* prologue;

    /* Composition mode draws the text as a centred column, so the side margins
     * stop being a constant and have to follow the pane's width. */
    gboolean composing;

    GtkTextTag* inline_tags[INLINE_TAG_COUNT];
    GtkTextTag* heading_tags[MAX_HEADING_LEVEL];
    GtkTextTag* quote_tag;
    GtkTextTag* code_block_tag;
    GtkTextTag* bullet_tag;
    GtkTextTag* ordered_tag;
    GtkTextTag* marker_tag;   /* list markers: shown, but not part of the text */

    EditorModifiedFn modified_callback;
    void*            modified_user_data;

    EditorStylesFn styles_callback;
    void*          styles_user_data;

    EditorBlockFn block_callback;
    void*         block_user_data;

    /* A style the author has asked for with nothing selected, waiting for
     * something to be typed into it: `asked_mask` is which styles have an
     * answer here, `asked_flags` is the answer. Two words rather than one
     * because "off" has to be sayable — Ctrl+B at the end of a bold word means
     * stop, and a single set of bits could not tell that from silence.
     *
     * It is asked for at a place rather than for a stretch of time, so any
     * cursor move forgets it, and the text carries it once something has been
     * typed. `inserting` and `insert_from` are that insertion in progress. */
    uint32_t asked_mask;
    uint32_t asked_flags;
    gboolean inserting;
    int      insert_from;      /* offset the text landed at */
    uint32_t insert_styles;    /* what it should come out wearing */

    /* Which block the text is landing in, read before it lands for the same
     * reason the inline styles are: at the end of a heading the tag stops
     * short, so GTK would hand back a plain character exactly where the author
     * is carrying on writing a heading.
     *
     * `return_action` is the other thing only readable from before: whether
     * this insertion is a return that carries a list on, or ends one. */
    EditorBlockStyle   insert_block;
    EditorReturnAction return_action;

    /* Undo. The store is borrowed and outlives the open document, keyed by its
     * path; see undo-stack.h for why a history is per item rather than per
     * window. `applying` is the guard that keeps an undo from being recorded as
     * a fresh edit, in the same idiom as `loading` above. `pending_delete` is
     * a deletion captured before the text went, waiting for it to be gone. */
    UndoStore*  undo;
    UndoRecord* pending_delete;
    gboolean    applying;

    /* Where the cursor was left by the last list a typed marker made, or -1.
     * Backspace there means "no, I meant a hyphen"; anything else at all
     * forgets it. See take_back_autoformat(). */
    int autoformat_at;

    EditorHistoryFn history_callback;
    void*           history_user_data;

    /* The red lines under the misspelled words. Owned, and follows the buffer's
     * own signals from here on; the panel only says when it is loading. */
    SpellCheck* spelling;
};

/* Defined with the rest of the styling, but needed by everything that leaves
 * the cursor somewhere new. */
static void notify_format(EditorPanel* editor);
static void forget_asked_styles(EditorPanel* editor);

/* Defined with the recording, but needed by the block styling above it: a pick
 * is captured before it lands, the same as a press of Bold. */
static gboolean recording(EditorPanel* editor);
static void     record_edit(EditorPanel* editor, UndoRecord* record);

/* Record what the open document's buffer looks like as it is put away, so that
 * a history coming back can be checked against the text it claims to describe.
 * Everything that takes a document off the screen calls this. */
static void note_leaving(EditorPanel* editor)
{
    if (editor->undo != NULL && editor->path != NULL) {
        undo_store_note_fingerprint(editor->undo, editor->path,
                                    undo_fingerprint(editor->buffer));
    }
}

/* ── tags ────────────────────────────────────────────────────────────────── */

/* Bit position of a single-bit span flag, so flags index into inline_tags. */
static int inline_tag_index(uint32_t span_flag)
{
    switch (span_flag) {
    case WORDSMITH_MARKUP_SPAN_EMPHASIS:  return 0;
    case WORDSMITH_MARKUP_SPAN_STRONG:    return 1;
    case WORDSMITH_MARKUP_SPAN_UNDERLINE: return 2;
    case WORDSMITH_MARKUP_SPAN_CODE:      return 3;
    default:                               return -1;
    }
}

static void build_tags(EditorPanel* editor)
{
    editor->inline_tags[0] = gtk_text_buffer_create_tag(
        editor->buffer, "emphasis", "style", PANGO_STYLE_ITALIC, NULL);
    editor->inline_tags[1] = gtk_text_buffer_create_tag(
        editor->buffer, "strong", "weight", PANGO_WEIGHT_BOLD, NULL);
    editor->inline_tags[2] = gtk_text_buffer_create_tag(
        editor->buffer, "underline", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    editor->inline_tags[3] = gtk_text_buffer_create_tag(
        editor->buffer, "code-span", "family", "monospace", NULL);

    /* Headings shrink as they nest: h1 is 1.6x body, h6 is body weight. */
    static const double HEADING_SCALES[MAX_HEADING_LEVEL] = {
        1.6, 1.4, 1.25, 1.15, 1.05, 1.0
    };
    for (int level = 0; level < MAX_HEADING_LEVEL; level++) {
        char name[16];
        g_snprintf(name, sizeof(name), "heading-%d", level + 1);
        editor->heading_tags[level] = gtk_text_buffer_create_tag(
            editor->buffer, name,
            "weight", PANGO_WEIGHT_BOLD,
            "scale", HEADING_SCALES[level],
            "pixels-above-lines", 12,
            "pixels-below-lines", 6,
            NULL);
    }

    editor->quote_tag = gtk_text_buffer_create_tag(
        editor->buffer, "quote",
        "style", PANGO_STYLE_ITALIC,
        "left-margin", 32,
        NULL);

    editor->code_block_tag = gtk_text_buffer_create_tag(
        editor->buffer, "code-block",
        "family", "monospace",
        "left-margin", 24,
        NULL);

    editor->bullet_tag = gtk_text_buffer_create_tag(
        editor->buffer, "list-bullet", "left-margin", 24, NULL);
    editor->ordered_tag = gtk_text_buffer_create_tag(
        editor->buffer, "list-ordered", "left-margin", 24, NULL);

    /* Markers are display scaffolding. Save skips text carrying this tag and
     * regenerates it from the list tags instead. */
    editor->marker_tag = gtk_text_buffer_create_tag(
        editor->buffer, "list-marker", "editable", FALSE, NULL);
}

/* ── loading ─────────────────────────────────────────────────────────────── */

/* Apply `tag` across the block that starts at `start_offset` and runs to the
 * buffer's current end. */
static void apply_to_block(EditorPanel* editor, GtkTextTag* tag, int start_offset)
{
    if (tag == NULL) {
        return;
    }
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(editor->buffer, &start, start_offset);
    gtk_text_buffer_get_end_iter(editor->buffer, &end);
    gtk_text_buffer_apply_tag(editor->buffer, tag, &start, &end);
}

static void insert_spans(EditorPanel* editor, const WordsmithMarkupDocument* doc,
                         size_t block)
{
    const size_t span_count = wordsmith_markup_block_span_count(doc, block);
    for (size_t index = 0; index < span_count; index++) {
        WordsmithMarkupSpan span = wordsmith_markup_block_span(doc, block, index);

        GtkTextIter end;
        gtk_text_buffer_get_end_iter(editor->buffer, &end);
        const int start_offset = gtk_text_iter_get_offset(&end);
        gtk_text_buffer_insert(editor->buffer, &end, span.text, -1);

        GtkTextIter start;
        gtk_text_buffer_get_iter_at_offset(editor->buffer, &start, start_offset);
        gtk_text_buffer_get_end_iter(editor->buffer, &end);

        for (int bit = 0; bit < INLINE_TAG_COUNT; bit++) {
            const uint32_t flag = 1u << bit;
            if ((span.flags & flag) != 0) {
                gtk_text_buffer_apply_tag(editor->buffer, editor->inline_tags[bit],
                                          &start, &end);
            }
        }
    }
}

static void insert_block(EditorPanel* editor, const WordsmithMarkupDocument* doc,
                         size_t block)
{
    const WordsmithMarkupBlockInfo info = wordsmith_markup_block_info(doc, block);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(editor->buffer, &end);
    const int start_offset = gtk_text_iter_get_offset(&end);

    switch (info.kind) {
    case WORDSMITH_MARKUP_HEADING: {
        int level = info.level < 1 ? 1 : info.level;
        if (level > MAX_HEADING_LEVEL) {
            level = MAX_HEADING_LEVEL;
        }
        insert_spans(editor, doc, block);
        apply_to_block(editor, editor->heading_tags[level - 1], start_offset);
        break;
    }

    case WORDSMITH_MARKUP_QUOTE:
        insert_spans(editor, doc, block);
        apply_to_block(editor, editor->quote_tag, start_offset);
        break;

    case WORDSMITH_MARKUP_CODE_BLOCK: {
        const char* code = wordsmith_markup_block_code(doc, block);
        gtk_text_buffer_insert(editor->buffer, &end, code, -1);
        apply_to_block(editor, editor->code_block_tag, start_offset);
        break;
    }

    case WORDSMITH_MARKUP_LIST_ITEM: {
        /* The marker is inserted as text so the list is legible, then tagged
         * so save() can tell it apart from what the author typed. */
        char marker[24];
        if (info.ordered) {
            g_snprintf(marker, sizeof(marker), "%d. ",
                       info.list_number > 0 ? info.list_number : 1);
        } else {
            g_strlcpy(marker, "- ", sizeof(marker));
        }
        gtk_text_buffer_insert(editor->buffer, &end, marker, -1);

        GtkTextIter marker_start;
        gtk_text_buffer_get_iter_at_offset(editor->buffer, &marker_start,
                                           start_offset);
        gtk_text_buffer_get_end_iter(editor->buffer, &end);
        gtk_text_buffer_apply_tag(editor->buffer, editor->marker_tag,
                                  &marker_start, &end);

        insert_spans(editor, doc, block);
        apply_to_block(editor,
                       info.ordered ? editor->ordered_tag : editor->bullet_tag,
                       start_offset);
        break;
    }

    case WORDSMITH_MARKUP_RULE:
        /* Rules have no spans. The visible text doubles as what save() reads
         * back, since a lone "---" line re-parses as a rule. */
        gtk_text_buffer_insert(editor->buffer, &end, "---", -1);
        break;

    case WORDSMITH_MARKUP_PARAGRAPH:
        insert_spans(editor, doc, block);
        break;
    }
}

int editor_panel_load(EditorPanel* editor, const char* path, char** error)
{
    char* text = wordsmith_document_read(path, error);
    if (text == NULL) {
        return 0;
    }

    /* Split off the frontmatter before the Markdown parser ever sees it: a
     * `---` fence is also a valid thematic break, and only the core knows
     * which one this is. */
    WordsmithFrontmatter* frontmatter = wordsmith_frontmatter_parse(text);
    size_t body = 0;
    if (frontmatter != NULL) {
        body = wordsmith_frontmatter_body_offset(frontmatter);
        wordsmith_frontmatter_free(frontmatter);
    }

    WordsmithMarkupDocument* doc = wordsmith_markup_parse(text + body);
    if (doc == NULL) {
        /* Nothing has been swapped in yet, so the open document is still
         * whole. */
        wordsmith_free_string(text);
        if (error != NULL) {
            *error = g_strdup("out of memory parsing document");
        }
        return 0;
    }

    g_free(editor->prologue);
    editor->prologue = g_strndup(text, body);
    wordsmith_free_string(text);

    /* Last look at the outgoing document, while its text is still here to be
     * described. Nothing has been swapped in before this point, so a document
     * that failed to parse has not disturbed the one it did not replace. */
    note_leaving(editor);

    /* One document arriving, not several hundred insertions: without this the
     * same lines would be checked once per block as the buffer filled. The
     * release at the other end is what checks the document, once. */
    spell_check_hold(editor->spelling);

    editor->loading = TRUE;
    gtk_text_buffer_set_text(editor->buffer, "", 0);

    const size_t block_count = wordsmith_markup_block_count(doc);
    for (size_t block = 0; block < block_count; block++) {
        if (block > 0) {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(editor->buffer, &end);
            gtk_text_buffer_insert(editor->buffer, &end, "\n", 1);
        }
        insert_block(editor, doc, block);
    }

    wordsmith_markup_document_free(doc);

    g_free(editor->path);
    editor->path = g_strdup(path);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(editor->buffer, &start);
    gtk_text_buffer_place_cursor(editor->buffer, &start);
    gtk_text_buffer_set_modified(editor->buffer, FALSE);
    editor->loading = FALSE;
    spell_check_release(editor->spelling);

    gtk_widget_set_sensitive(GTK_WIDGET(editor->view), TRUE);

    /* And the first look at the incoming one. A history whose offsets no longer
     * describe what came back off disk is dropped here rather than replayed
     * into the wrong places; see undo-stack.h. */
    if (editor->undo != NULL) {
        undo_store_check_fingerprint(editor->undo, editor->path,
                                     undo_fingerprint(editor->buffer));
    }

    forget_asked_styles(editor);
    notify_format(editor);
    return 1;
}

void editor_panel_refresh_frontmatter(EditorPanel* editor)
{
    if (editor == NULL || editor->path == NULL) {
        return;
    }

    char* error = NULL;
    char* text  = wordsmith_document_read(editor->path, &error);
    if (text == NULL) {
        /* The buffer still holds the body, so keeping the prologue we have is
         * the outcome that loses least. */
        wordsmith_free_string(error);
        return;
    }

    WordsmithFrontmatter* frontmatter = wordsmith_frontmatter_parse(text);
    size_t body = 0;
    if (frontmatter != NULL) {
        body = wordsmith_frontmatter_body_offset(frontmatter);
        wordsmith_frontmatter_free(frontmatter);
    }

    g_free(editor->prologue);
    editor->prologue = g_strndup(text, body);
    wordsmith_free_string(text);
}

/* ── saving ──────────────────────────────────────────────────────────────── */

/* Which block kind a line carries, read from the tags at its first character. */
typedef struct LineKind {
    WordsmithMarkupBlockKind kind;
    int                       level;
} LineKind;

static LineKind line_kind(EditorPanel* editor, const GtkTextIter* line_start)
{
    LineKind out = { WORDSMITH_MARKUP_PARAGRAPH, 0 };

    for (int level = 0; level < MAX_HEADING_LEVEL; level++) {
        if (gtk_text_iter_has_tag(line_start, editor->heading_tags[level])) {
            out.kind  = WORDSMITH_MARKUP_HEADING;
            out.level = level + 1;
            return out;
        }
    }
    if (gtk_text_iter_has_tag(line_start, editor->code_block_tag)) {
        out.kind = WORDSMITH_MARKUP_CODE_BLOCK;
        return out;
    }
    if (gtk_text_iter_has_tag(line_start, editor->quote_tag)) {
        out.kind = WORDSMITH_MARKUP_QUOTE;
        return out;
    }
    if (gtk_text_iter_has_tag(line_start, editor->ordered_tag)) {
        out.kind    = WORDSMITH_MARKUP_LIST_ITEM;
        out.level   = 1;   /* ordered, recorded in `level` for the caller */
        return out;
    }
    if (gtk_text_iter_has_tag(line_start, editor->bullet_tag)) {
        out.kind  = WORDSMITH_MARKUP_LIST_ITEM;
        out.level = 0;
        return out;
    }
    return out;
}

/* Emit the spans between `start` and `end`, splitting wherever the set of
 * inline tags changes. Marker text is skipped.
 *
 * The walk steps to the next toggle of *any* tag, because asking for the next
 * toggle of each of the four inline tags in turn would cost four steps to learn
 * the same thing. The price is that a tag with nothing to do with the markup
 * splits a run as readily as bold does — the red line under a misspelled word
 * is one such — so the text is gathered up and only handed over when the flags
 * actually change. Two spans with the same flags would otherwise each get their
 * own delimiters, and a bold word with a squiggle under half of it would save
 * as `**wo****rd**`: the same words, correctly, and a diff in the author's file
 * every time they opened it. */
static void add_spans_for_range(EditorPanel* editor,
                                WordsmithMarkupBuilder* builder,
                                const GtkTextIter* start, const GtkTextIter* end)
{
    GtkTextIter cursor        = *start;
    GString*    pending       = g_string_new(NULL);
    uint32_t    pending_flags = 0;

    while (gtk_text_iter_compare(&cursor, end) < 0) {
        GtkTextIter run_end = cursor;
        if (!gtk_text_iter_forward_to_tag_toggle(&run_end, NULL)
            || gtk_text_iter_compare(&run_end, end) > 0) {
            run_end = *end;
        }
        if (gtk_text_iter_compare(&run_end, &cursor) <= 0) {
            break;
        }

        if (!gtk_text_iter_has_tag(&cursor, editor->marker_tag)) {
            uint32_t flags = 0;
            for (int bit = 0; bit < INLINE_TAG_COUNT; bit++) {
                if (gtk_text_iter_has_tag(&cursor, editor->inline_tags[bit])) {
                    flags |= 1u << bit;
                }
            }

            if (pending->len > 0 && flags != pending_flags) {
                wordsmith_markup_builder_add_span(builder, pending->str,
                                                  pending_flags, NULL);
                g_string_truncate(pending, 0);
            }
            pending_flags = flags;

            char* text = gtk_text_buffer_get_text(editor->buffer, &cursor, &run_end,
                                                  FALSE);
            if (text != NULL) {
                g_string_append(pending, text);
            }
            g_free(text);
        }

        cursor = run_end;
    }

    if (pending->len > 0) {
        wordsmith_markup_builder_add_span(builder, pending->str, pending_flags, NULL);
    }
    g_string_free(pending, TRUE);
}

/* Whether the line beginning at `line_start` holds nothing but its own
 * scaffolding: no characters at all, or none that are not a list marker. */
static gboolean line_is_empty(EditorPanel* editor, const GtkTextIter* line_start)
{
    GtkTextIter cursor = *line_start;
    if (gtk_text_iter_ends_line(&cursor)) {
        return TRUE;
    }

    if (gtk_text_iter_has_tag(&cursor, editor->marker_tag)) {
        GtkTextIter line_end = cursor;
        gtk_text_iter_forward_to_line_end(&line_end);
        if (!gtk_text_iter_forward_to_tag_toggle(&cursor, editor->marker_tag)
            || gtk_text_iter_compare(&cursor, &line_end) >= 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Gather the run of code-block lines starting at `line`, emit it as one block,
 * and return the first line after it. */
static int emit_code_block(EditorPanel* editor, WordsmithMarkupBuilder* builder,
                           int line, int line_count)
{
    GString* code = g_string_new(NULL);
    int current = line;

    while (current < line_count) {
        GtkTextIter start;
        gtk_text_buffer_get_iter_at_line(editor->buffer, &start, current);
        if (line_kind(editor, &start).kind != WORDSMITH_MARKUP_CODE_BLOCK) {
            break;
        }

        GtkTextIter end = start;
        if (!gtk_text_iter_ends_line(&end)) {
            gtk_text_iter_forward_to_line_end(&end);
        }
        char* text = gtk_text_buffer_get_text(editor->buffer, &start, &end, FALSE);
        if (current > line) {
            g_string_append_c(code, '\n');
        }
        g_string_append(code, text != NULL ? text : "");
        g_free(text);
        current++;
    }

    wordsmith_markup_builder_begin_block(builder, WORDSMITH_MARKUP_CODE_BLOCK, 0);
    wordsmith_markup_builder_set_code(builder, code->str, NULL);
    g_string_free(code, TRUE);
    return current;
}

int editor_panel_save(EditorPanel* editor, char** error)
{
    if (editor->path == NULL) {
        return 1;
    }

    WordsmithMarkupBuilder* builder = wordsmith_markup_builder_new();
    if (builder == NULL) {
        if (error != NULL) {
            *error = g_strdup("out of memory building document");
        }
        return 0;
    }

    const int line_count = gtk_text_buffer_get_line_count(editor->buffer);
    int line = 0;
    int list_number = 1;
    gboolean previous_was_ordered = FALSE;

    while (line < line_count) {
        GtkTextIter start;
        gtk_text_buffer_get_iter_at_line(editor->buffer, &start, line);
        GtkTextIter end = start;
        if (!gtk_text_iter_ends_line(&end)) {
            gtk_text_iter_forward_to_line_end(&end);
        }

        const LineKind kind = line_kind(editor, &start);

        if (kind.kind == WORDSMITH_MARKUP_CODE_BLOCK) {
            line = emit_code_block(editor, builder, line, line_count);
            previous_was_ordered = FALSE;
            continue;
        }

        char* raw = gtk_text_buffer_get_text(editor->buffer, &start, &end, FALSE);
        const gboolean is_rule = raw != NULL && strcmp(raw, "---") == 0;
        g_free(raw);

        /* A block with nothing in it is not content, whatever kind it is.
         *
         * For a paragraph that is the author's spacing, and the serializer puts
         * a blank line between blocks already. For the rest it is a style asked
         * for on a line nothing has been typed into yet — picking Heading 1 on
         * an empty line and saving before typing would otherwise commit a bare
         * `#` and a trailing space to the manuscript. Nothing is lost: there
         * were no words there to lose.
         *
         * A list item's marker does not count as content. It is display rather
         * than text — save skips it and regenerates it — so an item with no
         * words is as empty as a blank line is. */
        const gboolean blank = line_is_empty(editor, &start);

        if (blank) {
            previous_was_ordered = FALSE;
            line++;
            continue;
        }

        if (is_rule && kind.kind == WORDSMITH_MARKUP_PARAGRAPH) {
            wordsmith_markup_builder_begin_block(builder, WORDSMITH_MARKUP_RULE, 0);
            previous_was_ordered = FALSE;
            line++;
            continue;
        }

        if (kind.kind == WORDSMITH_MARKUP_LIST_ITEM) {
            const gboolean ordered = kind.level == 1;
            if (!ordered || !previous_was_ordered) {
                list_number = 1;
            }
            wordsmith_markup_builder_begin_block(builder,
                                                  WORDSMITH_MARKUP_LIST_ITEM, 0);
            /* begin_block takes nesting depth in `level`; ordering and the
             * running number are set through the marker we regenerate here. */
            if (ordered) {
                char marker[24];
                g_snprintf(marker, sizeof(marker), "%d. ", list_number);
                list_number++;
                wordsmith_markup_builder_add_span(builder, marker, 0, NULL);
            } else {
                wordsmith_markup_builder_add_span(builder, "- ", 0, NULL);
            }
            previous_was_ordered = ordered;
        } else {
            wordsmith_markup_builder_begin_block(builder, kind.kind, kind.level);
            previous_was_ordered = FALSE;
        }

        add_spans_for_range(editor, builder, &start, &end);
        line++;
    }

    char* markdown = wordsmith_markup_builder_to_markdown(builder);
    wordsmith_markup_builder_free(builder);

    /* The frontmatter goes back exactly as it came in. */
    char* document = g_strconcat(editor->prologue != NULL ? editor->prologue : "",
                                 markdown != NULL ? markdown : "", NULL);
    wordsmith_free_string(markdown);

    const int ok = wordsmith_document_write(editor->path, document, error);
    g_free(document);

    if (ok) {
        gtk_text_buffer_set_modified(editor->buffer, FALSE);
    }
    return ok;
}

/* ── styling ─────────────────────────────────────────────────────────────── */

/* Whether `tag` runs unbroken across [start, end). Asked by the toggle, to
 * decide which way it goes, and by the report the format bar follows, so that a
 * lit button and the click that puts it out are answering the same question.
 *
 * The next toggle of the tag is where it stops, so this costs one step rather
 * than one per character — worth having on a path that runs on every cursor
 * move over a selection that may be the whole manuscript. */
static gboolean tag_covers(const GtkTextIter* start, const GtkTextIter* end,
                           GtkTextTag* tag)
{
    if (tag == NULL || !gtk_text_iter_has_tag(start, tag)) {
        return FALSE;
    }

    GtkTextIter cursor = *start;
    if (!gtk_text_iter_forward_to_tag_toggle(&cursor, tag)) {
        return TRUE;   /* on from here to the end of the buffer */
    }
    return gtk_text_iter_compare(&cursor, end) >= 0;
}

static uint32_t styles_in_range(const GtkTextIter* start, const GtkTextIter* end,
                                GtkTextTag* const* inline_tags, int count)
{
    uint32_t flags = 0;
    for (int bit = 0; bit < count; bit++) {
        if (tag_covers(start, end, inline_tags[bit])) {
            flags |= 1u << bit;
        }
    }
    return flags;
}

/* The one character an empty cursor answers for: the one behind it, which the
 * author has just typed past and would say they are "in". At the start of a
 * line there is nothing behind — the newline before it belongs to the line
 * above — so the character ahead answers instead. */
static void character_beside(const GtkTextIter* at, GtkTextIter* start,
                             GtkTextIter* end)
{
    *start = *at;
    if (!gtk_text_iter_starts_line(start)) {
        gtk_text_iter_backward_char(start);
    }
    *end = *start;
    gtk_text_iter_forward_char(end);
}

uint32_t editor_style_flags(GtkTextBuffer* buffer, GtkTextTag* const* inline_tags,
                            int count)
{
    if (buffer == NULL || inline_tags == NULL) {
        return 0;
    }

    GtkTextIter start;
    GtkTextIter end;
    if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_mark(buffer, &at,
                                         gtk_text_buffer_get_insert(buffer));
        character_beside(&at, &start, &end);
    }

    return styles_in_range(&start, &end, inline_tags, count);
}

uint32_t editor_typed_styles(uint32_t beside, uint32_t asked_mask,
                             uint32_t asked_flags)
{
    return (beside & ~asked_mask) | (asked_flags & asked_mask);
}

void editor_ask_for_style(uint32_t span_flag, uint32_t in_force,
                          uint32_t* asked_mask, uint32_t* asked_flags)
{
    if (asked_mask == NULL || asked_flags == NULL) {
        return;
    }

    *asked_mask |= span_flag;
    if ((in_force & span_flag) != 0) {
        *asked_flags &= ~span_flag;
    } else {
        *asked_flags |= span_flag;
    }
}

static void forget_asked_styles(EditorPanel* editor)
{
    editor->asked_mask  = 0;
    editor->asked_flags = 0;
}

uint32_t editor_panel_styles_at_cursor(EditorPanel* editor)
{
    if (editor == NULL || editor->path == NULL) {
        return 0;
    }
    return editor_typed_styles(
        editor_style_flags(editor->buffer, editor->inline_tags, INLINE_TAG_COUNT),
        editor->asked_mask, editor->asked_flags);
}

/* Both halves of what the format bar shows, off the one event. The bar's
 * buttons and its dropdown are answering about the same place, so they are
 * told about it at the same time and cannot drift a keystroke apart. */
static void notify_format(EditorPanel* editor)
{
    if (editor->styles_callback != NULL) {
        editor->styles_callback(editor_panel_styles_at_cursor(editor),
                                editor->styles_user_data);
    }
    if (editor->block_callback != NULL) {
        editor->block_callback(editor_panel_block_style_at_cursor(editor),
                               editor->block_user_data);
    }
}

/* A style changes tags without moving a character, and GtkTextBuffer only calls
 * itself modified when text moves. Nothing would otherwise mark the document as
 * needing a save, so bolding a word and closing the project would lose the
 * bold and the title bar would never say so. Everything that changes a tag on
 * the author's behalf says so here. */
static void note_style_change(EditorPanel* editor)
{
    gtk_text_buffer_set_modified(editor->buffer, TRUE);
}

/* ── block styling ───────────────────────────────────────────────────────── */

/* Name and identifier in EditorBlockStyle order. One table, because the
 * dropdown, the Format menu, the accelerators and Edit ▸ Undo's wording all
 * have to agree, and four copies of a list are four chances to disagree. */
static const struct BlockStyleNames {
    const char* name;
    const char* id;
} BLOCK_STYLES[EDITOR_BLOCK_STYLE_COUNT] = {
    [EDITOR_BLOCK_PARAGRAPH]      = { "Paragraph",     "paragraph"      },
    [EDITOR_BLOCK_HEADING_1]      = { "Heading 1",     "heading-1"      },
    [EDITOR_BLOCK_HEADING_2]      = { "Heading 2",     "heading-2"      },
    [EDITOR_BLOCK_HEADING_3]      = { "Heading 3",     "heading-3"      },
    [EDITOR_BLOCK_QUOTE]          = { "Block Quote",   "quote"          },
    [EDITOR_BLOCK_BULLET_LIST]    = { "Bulleted List", "bulleted-list"  },
    [EDITOR_BLOCK_NUMBERED_LIST]  = { "Numbered List", "numbered-list"  },
};

static gboolean block_style_offered(EditorBlockStyle style)
{
    return style >= 0 && style < EDITOR_BLOCK_STYLE_COUNT;
}

const char* editor_block_style_name(EditorBlockStyle style)
{
    return block_style_offered(style) ? BLOCK_STYLES[style].name : NULL;
}

const char* editor_block_style_id(EditorBlockStyle style)
{
    return block_style_offered(style) ? BLOCK_STYLES[style].id : NULL;
}

EditorBlockStyle editor_block_style_from_id(const char* id)
{
    if (id == NULL) {
        return EDITOR_BLOCK_OTHER;
    }
    for (int index = 0; index < EDITOR_BLOCK_STYLE_COUNT; index++) {
        if (strcmp(id, BLOCK_STYLES[index].id) == 0) {
            return (EditorBlockStyle) index;
        }
    }
    return EDITOR_BLOCK_OTHER;
}

static gboolean block_style_is_list(EditorBlockStyle style)
{
    return style == EDITOR_BLOCK_BULLET_LIST || style == EDITOR_BLOCK_NUMBERED_LIST;
}

/* The panel's own tags, gathered into the shape the rules below take. Built
 * per call rather than kept: it is a handful of pointer copies, and one place
 * deciding which tag answers for which style is worth more than the saving. */
static void block_tags(EditorPanel* editor, EditorBlockTags* out)
{
    memset(out, 0, sizeof(*out));

    out->styles[EDITOR_BLOCK_HEADING_1]     = editor->heading_tags[0];
    out->styles[EDITOR_BLOCK_HEADING_2]     = editor->heading_tags[1];
    out->styles[EDITOR_BLOCK_HEADING_3]     = editor->heading_tags[2];
    out->styles[EDITOR_BLOCK_QUOTE]         = editor->quote_tag;
    out->styles[EDITOR_BLOCK_BULLET_LIST]   = editor->bullet_tag;
    out->styles[EDITOR_BLOCK_NUMBERED_LIST] = editor->ordered_tag;

    int next = 0;
    out->unoffered[next++] = editor->code_block_tag;
    for (int level = 3; level < MAX_HEADING_LEVEL; level++) {
        out->unoffered[next++] = editor->heading_tags[level];
    }

    out->marker = editor->marker_tag;
}

/* A line and its newline. The newline is in the range deliberately: it is the
 * only character an empty line has, so a block tag that stopped short of it
 * would have nowhere to live on the line the author has just asked to make a
 * heading and is about to type into. */
static void line_bounds(GtkTextBuffer* buffer, int line, GtkTextIter* start,
                        GtkTextIter* end)
{
    gtk_text_buffer_get_iter_at_line(buffer, start, line);
    *end = *start;
    if (!gtk_text_iter_forward_line(end)) {
        gtk_text_buffer_get_end_iter(buffer, end);
    }
}

/* A line without its newline: what the author would say is written there. The
 * other half of the pair above, and the two are not interchangeable — a block
 * tag has to reach the newline, and a line's *text* stops before it. */
static void line_text_bounds(GtkTextBuffer* buffer, int line, GtkTextIter* start,
                             GtkTextIter* end)
{
    gtk_text_buffer_get_iter_at_line(buffer, start, line);
    *end = *start;
    if (!gtk_text_iter_ends_line(end)) {
        gtk_text_iter_forward_to_line_end(end);
    }
}

/* The marker at the head of `line`, as [start, end). Both land on the line's
 * first character when there is none. */
static void marker_bounds(GtkTextBuffer* buffer, const EditorBlockTags* tags,
                          int line, GtkTextIter* start, GtkTextIter* end)
{
    gtk_text_buffer_get_iter_at_line(buffer, start, line);
    *end = *start;

    if (tags->marker == NULL || !gtk_text_iter_has_tag(start, tags->marker)) {
        return;
    }
    if (!gtk_text_iter_forward_to_tag_toggle(end, tags->marker)) {
        *end = *start;
        return;
    }

    /* A marker cannot reach past its own line, however the tag runs. */
    GtkTextIter line_end = *start;
    if (!gtk_text_iter_ends_line(&line_end)) {
        gtk_text_iter_forward_to_line_end(&line_end);
    }
    if (gtk_text_iter_compare(end, &line_end) > 0) {
        *end = line_end;
    }
}

EditorBlockStyle editor_block_style_at(GtkTextBuffer* buffer,
                                       const EditorBlockTags* tags, int line)
{
    if (buffer == NULL || tags == NULL || line < 0
        || line >= gtk_text_buffer_get_line_count(buffer)) {
        return EDITOR_BLOCK_OTHER;
    }

    GtkTextIter at;
    gtk_text_buffer_get_iter_at_line(buffer, &at, line);

    for (int index = 0; index < (int) G_N_ELEMENTS(tags->unoffered); index++) {
        if (tags->unoffered[index] == NULL) {
            break;
        }
        if (gtk_text_iter_has_tag(&at, tags->unoffered[index])) {
            return EDITOR_BLOCK_OTHER;
        }
    }

    /* Paragraph is the absence of the rest, so it is the answer nothing else
     * claimed rather than a tag to look for. */
    for (int index = EDITOR_BLOCK_PARAGRAPH + 1; index < EDITOR_BLOCK_STYLE_COUNT;
         index++) {
        if (tags->styles[index] != NULL
            && gtk_text_iter_has_tag(&at, tags->styles[index])) {
            return (EditorBlockStyle) index;
        }
    }
    return EDITOR_BLOCK_PARAGRAPH;
}

/* Put `text` at the head of `line` as a marker. */
static void insert_marker(GtkTextBuffer* buffer, const EditorBlockTags* tags,
                          int line, const char* text)
{
    if (tags->marker == NULL) {
        return;
    }
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_line(buffer, &at, line);
    gtk_text_buffer_insert_with_tags(buffer, &at, text, -1, tags->marker, NULL);
}

static void remove_marker(GtkTextBuffer* buffer, const EditorBlockTags* tags,
                          int line)
{
    GtkTextIter start;
    GtkTextIter end;
    marker_bounds(buffer, tags, line, &start, &end);
    if (gtk_text_iter_compare(&start, &end) < 0) {
        gtk_text_buffer_delete(buffer, &start, &end);
    }
}

void editor_block_apply(GtkTextBuffer* buffer, const EditorBlockTags* tags,
                        int first_line, int last_line, EditorBlockStyle style)
{
    if (buffer == NULL || tags == NULL || !block_style_offered(style)) {
        return;
    }

    const int line_count = gtk_text_buffer_get_line_count(buffer);
    if (first_line < 0) {
        first_line = 0;
    }
    if (last_line >= line_count) {
        last_line = line_count - 1;
    }

    for (int line = first_line; line <= last_line; line++) {
        remove_marker(buffer, tags, line);

        GtkTextIter start;
        GtkTextIter end;
        line_bounds(buffer, line, &start, &end);
        for (int index = 0; index < EDITOR_BLOCK_STYLE_COUNT; index++) {
            if (tags->styles[index] != NULL) {
                gtk_text_buffer_remove_tag(buffer, tags->styles[index], &start, &end);
            }
        }
        for (int index = 0; index < (int) G_N_ELEMENTS(tags->unoffered); index++) {
            if (tags->unoffered[index] == NULL) {
                break;
            }
            gtk_text_buffer_remove_tag(buffer, tags->unoffered[index], &start, &end);
        }

        if (block_style_is_list(style)) {
            insert_marker(buffer, tags, line,
                          style == EDITOR_BLOCK_NUMBERED_LIST ? "1. " : "- ");
        }

        /* Re-read: a marker just moved the end of the line, and the tag has to
         * cover it the way a loaded list item's does. */
        line_bounds(buffer, line, &start, &end);
        if (tags->styles[style] != NULL) {
            gtk_text_buffer_apply_tag(buffer, tags->styles[style], &start, &end);
        }
    }
}

void editor_block_renumber(GtkTextBuffer* buffer, const EditorBlockTags* tags,
                           int first_line, int last_line)
{
    if (buffer == NULL || tags == NULL
        || tags->styles[EDITOR_BLOCK_NUMBERED_LIST] == NULL
        || tags->marker == NULL) {
        return;
    }

    const int line_count = gtk_text_buffer_get_line_count(buffer);
    if (first_line < 0) {
        first_line = 0;
    }
    if (last_line >= line_count) {
        last_line = line_count - 1;
    }
    if (first_line > last_line) {
        return;
    }

    /* A run clipped by the range is still one run: reach out to both its ends,
     * or inserting an item in the middle of ten would number from there. */
    while (first_line > 0
           && editor_block_style_at(buffer, tags, first_line - 1)
                  == EDITOR_BLOCK_NUMBERED_LIST) {
        first_line--;
    }
    while (last_line + 1 < line_count
           && editor_block_style_at(buffer, tags, last_line + 1)
                  == EDITOR_BLOCK_NUMBERED_LIST) {
        last_line++;
    }

    int number = 1;
    for (int line = first_line; line <= last_line; line++) {
        if (editor_block_style_at(buffer, tags, line) != EDITOR_BLOCK_NUMBERED_LIST) {
            number = 1;
            continue;
        }

        char wanted[24];
        g_snprintf(wanted, sizeof(wanted), "%d. ", number);
        number++;

        GtkTextIter start;
        GtkTextIter end;
        marker_bounds(buffer, tags, line, &start, &end);
        char* current = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        const gboolean same = current != NULL && strcmp(current, wanted) == 0;
        g_free(current);
        if (same) {
            continue;
        }

        if (gtk_text_iter_compare(&start, &end) < 0) {
            gtk_text_buffer_delete(buffer, &start, &end);
        }
        insert_marker(buffer, tags, line, wanted);

        line_bounds(buffer, line, &start, &end);
        gtk_text_buffer_apply_tag(buffer, tags->styles[EDITOR_BLOCK_NUMBERED_LIST],
                                  &start, &end);
    }
}

/* The lines a press addresses: every one the selection touches, or the
 * cursor's. A selection ending exactly at the start of a line has not reached
 * into it — dragging down to the next paragraph should not take it too. */
static void block_lines_at_cursor(GtkTextBuffer* buffer, int* first, int* last)
{
    GtkTextIter start;
    GtkTextIter end;
    if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(buffer, &start,
                                         gtk_text_buffer_get_insert(buffer));
        end = start;
    }

    *first = gtk_text_iter_get_line(&start);
    *last  = gtk_text_iter_get_line(&end);
    if (*last > *first && gtk_text_iter_starts_line(&end)) {
        (*last)--;
    }
}

/* Set every line in `lines` to the kind it is meant to end up as, `reverse`
 * choosing which end of the record that is. The one path a pick and an undo of
 * a pick both take, so neither can grow a rule the other lacks.
 *
 * The markers this puts in and takes out belong to the block record, not to the
 * text history: one pick is one press to take back, so `applying` keeps the
 * insertions from arriving as edits of their own. */
static void apply_block_lines(EditorPanel* editor, const UndoBlockLine* lines,
                              size_t line_count, gboolean reverse)
{
    if (line_count == 0) {
        return;
    }

    EditorBlockTags tags;
    block_tags(editor, &tags);

    const gboolean was_applying = editor->applying;
    editor->applying = TRUE;

    int first = lines[0].line;
    int last  = lines[0].line;
    for (size_t index = 0; index < line_count; index++) {
        const UndoBlockLine* entry = &lines[index];
        editor_block_apply(editor->buffer, &tags, entry->line, entry->line,
                           (EditorBlockStyle) (reverse ? entry->before
                                                       : entry->after));
        first = MIN(first, entry->line);
        last  = MAX(last, entry->line);
    }

    /* One pass at the end rather than one per line: a numbered run reaches past
     * whatever was touched, and renumbering it once is both cheaper and the
     * only way the count comes out right. */
    editor_block_renumber(editor->buffer, &tags, first - 1, last + 1);

    editor->applying = was_applying;
}

/* The one kind covering [first_line, last_line], or EDITOR_BLOCK_OTHER when
 * they are not all the same. Only an answer covering the whole of what is
 * addressed counts, which is the rule the inline report follows over a
 * selection — and it has the same two jobs, since it is both what the bar draws
 * and what a press is folded against. */
static EditorBlockStyle block_style_over(GtkTextBuffer* buffer,
                                         const EditorBlockTags* tags,
                                         int first_line, int last_line)
{
    const EditorBlockStyle style = editor_block_style_at(buffer, tags, first_line);
    for (int line = first_line + 1; line <= last_line; line++) {
        if (editor_block_style_at(buffer, tags, line) != style) {
            return EDITOR_BLOCK_OTHER;
        }
    }
    return style;
}

EditorReturnAction editor_return_action(const char* text, int length,
                                        EditorBlockStyle line_style,
                                        gboolean line_is_bare)
{
    if (length != 1 || text == NULL || text[0] != '\n'
        || !block_style_is_list(line_style)) {
        return EDITOR_RETURN_ORDINARY;
    }
    return line_is_bare ? EDITOR_RETURN_END_LIST : EDITOR_RETURN_CONTINUE_LIST;
}

/* Whether a typed marker makes a list, read from the config file the first time
 * anything asks. Lazily rather than through an init call beside
 * spell_check_init(): there is nothing to build, one boolean with one reader,
 * and a file read on the first keystroke of a sitting is cheaper than an entry
 * point every caller of this file has to remember to call. -1 is "not asked
 * yet", which is why this is not a gboolean. */
static int autoformat_lists = -1;

gboolean editor_autoformat_lists(void)
{
    if (autoformat_lists < 0) {
        autoformat_lists = wordsmith_preferences_autoformat_lists() != 0;
    }
    return autoformat_lists != 0;
}

int editor_autoformat_set_lists(gboolean wanted, char** error)
{
    /* Before the write, and whatever the write does: a preference that could not
     * be saved is still the answer the author just gave. */
    autoformat_lists = wanted ? 1 : 0;
    return wordsmith_preferences_set_autoformat_lists(wanted ? 1 : 0, error);
}

/* What a line has to read for the space just typed to have been a list being
 * started. The same strings editor_block_apply() draws a marker with, which is
 * not a coincidence worth factoring out: one is what an author types and the
 * other is what a list looks like, and they are free to differ. */
static const struct {
    const char*      typed;
    EditorBlockStyle style;
} LIST_MARKERS[] = {
    { "- ", EDITOR_BLOCK_BULLET_LIST },
    { "1. ", EDITOR_BLOCK_NUMBERED_LIST },
};

EditorBlockStyle editor_autoformat_style(const char* text, int length,
                                         const char* line_text,
                                         EditorBlockStyle line_style)
{
    if (length != 1 || text == NULL || text[0] != ' ' || line_text == NULL
        || line_style != EDITOR_BLOCK_PARAGRAPH) {
        return EDITOR_BLOCK_OTHER;
    }

    for (gsize index = 0; index < G_N_ELEMENTS(LIST_MARKERS); index++) {
        if (strcmp(line_text, LIST_MARKERS[index].typed) == 0) {
            return LIST_MARKERS[index].style;
        }
    }
    return EDITOR_BLOCK_OTHER;
}

EditorBlockStyle editor_block_style_for_press(EditorBlockStyle style,
                                              EditorBlockStyle in_force)
{
    if (block_style_is_list(style) && in_force == style) {
        return EDITOR_BLOCK_PARAGRAPH;
    }
    return style;
}

EditorBlockStyle editor_panel_block_style_at_cursor(EditorPanel* editor)
{
    if (editor == NULL || editor->path == NULL) {
        return EDITOR_BLOCK_OTHER;
    }

    EditorBlockTags tags;
    block_tags(editor, &tags);

    int first = 0;
    int last  = 0;
    block_lines_at_cursor(editor->buffer, &first, &last);

    return block_style_over(editor->buffer, &tags, first, last);
}

void editor_panel_set_block_style(EditorPanel* editor, EditorBlockStyle style)
{
    if (editor == NULL || !block_style_offered(style)) {
        return;
    }

    /* A pick the editor does nothing with still gets an answer, because the
     * dropdown has already moved itself to show it. The buttons can be left
     * alone in this case — a toggle that was not acted on is still showing the
     * last true thing it was told — but a dropdown would sit there naming a
     * style for a document that is not open. Reporting puts it back. */
    if (editor->path == NULL) {
        notify_format(editor);
        return;
    }

    EditorBlockTags tags;
    block_tags(editor, &tags);

    int first = 0;
    int last  = 0;
    block_lines_at_cursor(editor->buffer, &first, &last);

    /* What the press actually means here — a list asked for where one already
     * covers the whole of what is addressed is a list being taken off. */
    const EditorBlockStyle wanted = editor_block_style_for_press(
        style, block_style_over(editor->buffer, &tags, first, last));

    /* What each line was, gathered before the press destroys it. Only the lines
     * that actually change go in, so undoing a pick over a mixed selection puts
     * every line back to its own kind rather than to one of them, and a line
     * the UI has no name for is passed over rather than taken somewhere no
     * record could bring it back from. */
    GArray* changed = g_array_new(FALSE, FALSE, sizeof(UndoBlockLine));
    for (int line = first; line <= last; line++) {
        const EditorBlockStyle before = editor_block_style_at(editor->buffer, &tags,
                                                              line);
        if (before == wanted || before == EDITOR_BLOCK_OTHER) {
            continue;
        }
        const UndoBlockLine entry = { line, before, wanted };
        g_array_append_val(changed, entry);
    }

    if (changed->len == 0) {
        g_array_free(changed, TRUE);
        /* Nothing to do, and the same reason to say so: the pick may have been
         * one the lines could not take, and the dropdown has moved to it. */
        notify_format(editor);
        return;
    }

    /* Named for what the author asked for rather than what the lines became, so
     * taking a list off reads "Undo Bulleted List" — the same way a style
     * record is called "Bold" whichever direction the press went. */
    UndoRecord* captured =
        recording(editor)
            ? undo_record_new_block(editor_block_style_name(style),
                                    (const UndoBlockLine*) changed->data,
                                    changed->len)
            : NULL;

    apply_block_lines(editor, (const UndoBlockLine*) changed->data, changed->len,
                      FALSE);
    g_array_free(changed, TRUE);

    note_style_change(editor);
    record_edit(editor, captured);
    notify_format(editor);
}

/* ── recording edits ─────────────────────────────────────────────────────── */

static void notify_history(EditorPanel* editor)
{
    if (editor->history_callback != NULL) {
        editor->history_callback(editor->history_user_data);
    }
}

/* Whether what is happening to the buffer is the author's doing. Loading and
 * applying an undo both move text about, and neither is an edit to remember. */
static gboolean recording(EditorPanel* editor)
{
    return editor->undo != NULL && editor->path != NULL && !editor->loading
        && !editor->applying;
}

/* Takes ownership, including when there is nowhere to put it. */
static void record_edit(EditorPanel* editor, UndoRecord* record)
{
    if (record == NULL) {
        return;
    }
    if (!recording(editor)) {
        undo_record_free(record);
        return;
    }

    undo_store_push(editor->undo, editor->path, record);
    notify_history(editor);
}

/* A deletion has to be read before it happens — afterwards the text and the
 * tags over it are both gone — so both ends of "delete-range" are connected,
 * the same shape as the insertion pair below. */
/* The spot a backspace would undo a conversion at is forgotten by anything else
 * happening at all — another keystroke, a deletion, the cursor moving. Only the
 * press *immediately* after the list made itself means "no"; once the author
 * has carried on, backspace is an ordinary backspace again. */
static void forget_autoformat(EditorPanel* editor)
{
    editor->autoformat_at = -1;
}

static void on_range_deleting(GtkTextBuffer* buffer, GtkTextIter* start,
                              GtkTextIter* end, gpointer user_data)
{
    EditorPanel* editor = user_data;
    if (!editor->applying) {
        forget_autoformat(editor);
    }

    g_clear_pointer(&editor->pending_delete, undo_record_free);
    if (!recording(editor)) {
        return;
    }
    editor->pending_delete = undo_record_capture_text(
        buffer, editor->inline_tags, INLINE_TAG_COUNT, UNDO_TEXT_DELETE,
        gtk_text_iter_get_offset(start), gtk_text_iter_get_offset(end));
}

static void on_range_deleted(GtkTextBuffer* buffer, GtkTextIter* start,
                             GtkTextIter* end, gpointer user_data)
{
    (void) buffer;
    (void) start;
    (void) end;

    EditorPanel* editor    = user_data;
    UndoRecord*  captured  = editor->pending_delete;
    editor->pending_delete = NULL;
    record_edit(editor, captured);
}

/* Typing, moving and selecting all move the insertion point, so one signal
 * covers every way the answer changes from the author's side. What this panel
 * does to the tags itself does not move the cursor, and says so directly.
 *
 * A style asked for with nothing selected is asked for *here*, so leaving takes
 * it back — otherwise Ctrl+B, a change of mind and a click three paragraphs
 * away would leave a bold word waiting there. The insertion the author is in
 * the middle of is the one move that does not count: the mark reaches its new
 * home before the text is styled, and it is that text they asked for. */
static void on_cursor_moved(GObject* buffer, GParamSpec* spec, gpointer user_data)
{
    (void) buffer;
    (void) spec;

    EditorPanel* editor = user_data;
    if (editor->inserting) {
        return;
    }

    /* And the run of typing ends here too, for the same reason the asked-for
     * style does: two stretches of typing with a click between them are two
     * things done, however close together the offsets happen to fall. */
    if (!editor->applying && editor->undo != NULL && editor->path != NULL) {
        undo_store_break_run(editor->undo, editor->path);
        forget_autoformat(editor);
    }

    forget_asked_styles(editor);
    notify_format(editor);
}

/* Whether any of the inline tags is already somewhere in [from, to).
 *
 * Text that arrives wearing something — a paste of formatted text, which GTK
 * restores tag by tag — is left as it is. Only text that turned up bare takes
 * the styling of the place it landed in. */
static gboolean range_is_styled(EditorPanel* editor, int from, int to)
{
    for (int bit = 0; bit < INLINE_TAG_COUNT; bit++) {
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(editor->buffer, &start, from);
        gtk_text_buffer_get_iter_at_offset(editor->buffer, &end, to);

        if (gtk_text_iter_has_tag(&start, editor->inline_tags[bit])) {
            return TRUE;
        }
        if (gtk_text_iter_forward_to_tag_toggle(&start, editor->inline_tags[bit])
            && gtk_text_iter_compare(&start, &end) < 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Before the text lands: work out what it should be wearing while the place it
 * is going still reads as it did. GTK gives inserted text the tags that cover
 * the spot, which is not the same question — at the end of a bold word the tag
 * stops short, and that is exactly where an author carries on typing in bold. */
/* Read the two facts about where the text is going that the return rule needs,
 * and ask it. Both have to be read *before* the newline lands: afterwards the
 * item is no longer the cursor's line, and no longer empty in the way that
 * matters. */
static EditorReturnAction return_action_at(EditorPanel* editor,
                                           const GtkTextIter* location,
                                           const char* text, int length)
{
    EditorBlockTags tags;
    block_tags(editor, &tags);

    const int   line = gtk_text_iter_get_line(location);
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_line(editor->buffer, &start, line);

    return editor_return_action(text, length,
                                editor_block_style_at(editor->buffer, &tags, line),
                                line_is_empty(editor, &start));
}

static void on_text_inserting(GtkTextBuffer* buffer, GtkTextIter* location,
                              char* text, int length, gpointer user_data)
{
    EditorPanel* editor = user_data;
    /* Applying a record dresses its own text — put_text_back() clears the
     * inline tags and lays the record's runs down instead — and the markers a
     * block record regenerates are not the author's typing either. */
    if (editor->loading || editor->applying) {
        return;
    }
    forget_autoformat(editor);

    /* What a return here means, decided while the line it was pressed on is
     * still the line the cursor is in. The after handler acts on a
     * continuation; ending a list has to be dealt with now, because it is the
     * one answer that refuses the newline — stopping the emission is what keeps
     * it from ever reaching the buffer. The line becomes a paragraph through
     * the ordinary verb, so it is one block record and one press to take back. */
    editor->return_action = return_action_at(editor, location, text, length);
    if (editor->return_action == EDITOR_RETURN_END_LIST) {
        g_signal_stop_emission_by_name(buffer, "insert-text");
        editor_panel_set_block_style(editor, EDITOR_BLOCK_PARAGRAPH);
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    character_beside(location, &start, &end);

    editor->insert_styles = editor_typed_styles(
        styles_in_range(&start, &end, editor->inline_tags, INLINE_TAG_COUNT),
        editor->asked_mask, editor->asked_flags);

    EditorBlockTags block;
    block_tags(editor, &block);
    editor->insert_block =
        editor_block_style_at(editor->buffer, &block,
                              gtk_text_iter_get_line(location));

    editor->insert_from = gtk_text_iter_get_offset(location);
    editor->inserting   = TRUE;
}

/* Give text that has just landed the block its line is, over [from, to).
 *
 * The same problem the inline styles have and the same shape of answer: GTK
 * hands inserted text the tags *covering* the spot, and at the end of a heading
 * the heading tag stops short, so carrying on typing a title used to produce
 * plain characters that save then read past. It is also what makes an empty
 * line stylable at all — the tag lives on the line's newline until there is
 * text to put it on, and this is what moves it onto the text.
 *
 * Only as far as the first newline in what arrived, which is what keeps Enter
 * doing what it did: the line a return opens is a paragraph, because the block
 * ends where the text does. A paste of several paragraphs into a heading takes
 * the heading on its first line and leaves the rest alone, for the same
 * reason. */
static void dress_block(EditorPanel* editor, int from, int to)
{
    if (to <= from || !block_style_offered(editor->insert_block)) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(editor->buffer, &start, from);
    gtk_text_buffer_get_iter_at_offset(editor->buffer, &end, to);

    /* Nothing landed on this line: what arrived begins with the newline, which
     * is a return being pressed. */
    if (gtk_text_iter_ends_line(&start)) {
        return;
    }
    GtkTextIter line_end = start;
    gtk_text_iter_forward_to_line_end(&line_end);
    if (gtk_text_iter_compare(&line_end, &end) < 0) {
        end = line_end;
    }

    EditorBlockTags tags;
    block_tags(editor, &tags);

    for (int index = 0; index < EDITOR_BLOCK_STYLE_COUNT; index++) {
        if (tags.styles[index] == NULL) {
            continue;
        }
        if (index == (int) editor->insert_block) {
            gtk_text_buffer_apply_tag(editor->buffer, tags.styles[index], &start,
                                      &end);
        } else {
            gtk_text_buffer_remove_tag(editor->buffer, tags.styles[index], &start,
                                       &end);
        }
    }
}

/* Backspace where a typed marker has just made a list: take the list back and
 * leave the characters, exactly as one Ctrl+Z there does.
 *
 * It has to be caught here rather than left to the buffer, because a marker is
 * tagged non-editable — a backspace against one does nothing at all, so an
 * author who typed `- ` and wanted a hyphen found the key they would reach for
 * first doing nothing, with no hint that Ctrl+Z was the way out. This is the
 * gesture every editor that formats while you type answers this way.
 *
 * The record on top of the history *is* the conversion, because `autoformat_at`
 * survives nothing else happening, so the edit goes through the same record a
 * press of Ctrl+Z would rather than being reversed by hand here. One rule, one
 * path, and the two cannot come to mean different things.
 *
 * The **caret** is the one thing the two do differently, and deliberately. Undo
 * lands it on the line it changed, which is where undoing any block pick lands
 * it; a backspace was pressed by someone whose caret was after the `- ` they
 * typed, and leaving it at the head of the line would make the next press join
 * the line to the one above instead of eating the marker they are in the middle
 * of removing.
 *
 * Returns whether the backspace was spent on this rather than on the text. */
static gboolean take_back_autoformat(EditorPanel* editor)
{
    GtkTextIter at;
    if (editor->autoformat_at < 0 || editor->undo == NULL || editor->path == NULL) {
        return FALSE;
    }
    gtk_text_buffer_get_iter_at_mark(editor->buffer, &at,
                                     gtk_text_buffer_get_insert(editor->buffer));
    if (gtk_text_iter_get_offset(&at) != editor->autoformat_at) {
        return FALSE;
    }

    const UndoRecord* record = undo_store_peek_undo(editor->undo, editor->path);
    if (record == NULL) {
        return FALSE;
    }

    /* Read before the record moves anything; the line survives, the offsets do
     * not. */
    const int line = gtk_text_iter_get_line(&at);

    editor_panel_apply_record(editor, record, TRUE);
    undo_store_step_undo(editor->undo, editor->path);
    notify_history(editor);
    forget_autoformat(editor);

    /* Back to the end of the characters that have just come back, which is the
     * far side of the `- ` the author is now holding Backspace down on. */
    GtkTextIter start;
    GtkTextIter end;
    line_text_bounds(editor->buffer, line, &start, &end);
    gtk_text_buffer_place_cursor(editor->buffer, &end);
    return TRUE;
}

static void on_backspace(GtkTextView* view, gpointer user_data)
{
    if (take_back_autoformat(user_data)) {
        /* Connected in the ordinary phase, so stopping the emission is what
         * keeps GtkTextView's own binding from deleting a character as well. */
        g_signal_stop_emission_by_name(view, "backspace");
    }
}

/* Put `location` back where the author is standing, after this panel has moved
 * text about inside its own "insert-text" handler.
 *
 * **Anything here that edits the buffer from that handler owes the handlers
 * behind it this call.** GTK hands every after-handler the same iterator, and
 * an iterator does not survive a mutation — so an insertion or a deletation of
 * ours leaves whoever is connected after us reading a dead one. spell-check.c
 * is exactly that: it is created after the panel's handlers on purpose, and it
 * reads `location` for the line to recheck and for where the author is typing.
 * Left alone, GTK warns and the marking reads a garbage offset.
 *
 * The insert mark is the right answer rather than an arithmetic correction of
 * the old offset: what the iterator means to anyone behind us is where the text
 * that just arrived has left the author, and after these edits that is exactly
 * where the cursor now is. */
static void revalidate(EditorPanel* editor, GtkTextIter* location)
{
    if (location == NULL) {
        return;
    }
    gtk_text_buffer_get_iter_at_mark(editor->buffer, location,
                                     gtk_text_buffer_get_insert(editor->buffer));
}

/* Open another item below the one a return was pressed in, the before handler
 * having already decided that is what it meant.
 *
 * The record says the new line was a **paragraph**, which is not quite what it
 * was — it did not exist. It is the truthful thing to say about a line the
 * other half of the compound is about to delete, and it is what makes undo
 * strip the marker before the newline goes: without it the marker would be left
 * behind as ordinary text in the manuscript.
 *
 * The style is applied whatever the new line already reads as. GTK gives the
 * inserted newline the tags covering the spot, so the line below is already
 * wearing the list tag and answers as an item — with no marker in front of it,
 * which is exactly the state this is here to fix.
 *
 * Returns the record for the item, or NULL when there was no item to open. */
static UndoRecord* continue_list(EditorPanel* editor, GtkTextIter* location, int to)
{
    if (editor->return_action != EDITOR_RETURN_CONTINUE_LIST
        || !block_style_is_list(editor->insert_block)) {
        return NULL;
    }

    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(editor->buffer, &at, to);
    const UndoBlockLine entry = { gtk_text_iter_get_line(&at),
                                  EDITOR_BLOCK_PARAGRAPH, editor->insert_block };

    UndoRecord* record =
        recording(editor)
            ? undo_record_new_block(editor_block_style_name(editor->insert_block),
                                    &entry, 1)
            : NULL;

    apply_block_lines(editor, &entry, 1, FALSE);
    revalidate(editor, location);
    return record;
}

/* Take the marker the author typed out of the text and make the line the list
 * it was asking for, editor_autoformat_style() having said that is what the
 * space meant.
 *
 * Two edits and one thing done, so the record is a compound — and a compound of
 * its own, pushed after the space's own text record rather than folded into it.
 * That is the whole of the escape hatch: one Ctrl+Z takes the conversion back
 * and leaves a paragraph reading `- `, so an author who meant a literal hyphen
 * carries on typing, and a second press takes the marker away as ordinary
 * typing. Rolling both into one record would leave them nothing to do but type
 * the same three characters again.
 *
 * **The list goes on first and the typed characters come out from behind it**,
 * which is the record's order as much as the buffer's. A compound is named for
 * its first part, and what the author did here was ask for a list — the other
 * way round it would read "Undo Typing", naming the bookkeeping instead of the
 * gesture. It also gives the two parts an order that is right in both
 * directions: undoing puts the typed marker back before the drawn one comes
 * off, so the line never ends up wearing neither. The block part being the last
 * one undone is what leaves the caret at the head of the line afterwards, which
 * is where undoing any block pick leaves it.
 *
 * Both buffer edits are made under `applying`, the way a pick's markers are, so
 * neither arrives in the history as an edit of its own. */
static void autoformat_list(EditorPanel* editor, GtkTextIter* location,
                            const char* text, int length)
{
    if (!editor_autoformat_lists()) {
        return;
    }

    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(editor->buffer, &cursor,
                                     gtk_text_buffer_get_insert(editor->buffer));
    const int line = gtk_text_iter_get_line(&cursor);

    GtkTextIter start;
    GtkTextIter end;
    line_text_bounds(editor->buffer, line, &start, &end);

    char* line_text = gtk_text_buffer_get_text(editor->buffer, &start, &end, FALSE);
    const EditorBlockStyle style =
        editor_autoformat_style(text, length, line_text, editor->insert_block);
    g_free(line_text);

    if (style == EDITOR_BLOCK_OTHER) {
        return;
    }

    const UndoBlockLine entry = { line, EDITOR_BLOCK_PARAGRAPH, style };
    UndoRecord*         parts[2] = { NULL, NULL };
    if (recording(editor)) {
        parts[0] = undo_record_new_block(editor_block_style_name(style), &entry, 1);
    }
    apply_block_lines(editor, &entry, 1, FALSE);

    /* What the author typed is now whatever follows the marker the list drew,
     * which is how it is found rather than by counting characters: the two
     * happen to be the same width and have no reason to stay that way. */
    EditorBlockTags tags;
    block_tags(editor, &tags);

    GtkTextIter head;
    marker_bounds(editor->buffer, &tags, line, &head, &start);
    line_text_bounds(editor->buffer, line, &head, &end);

    if (recording(editor)) {
        /* Before the characters go, for the reason every deletion is read on
         * the way out rather than after it. */
        parts[1] = undo_record_capture_text(editor->buffer, editor->inline_tags,
                                            INLINE_TAG_COUNT, UNDO_TEXT_DELETE,
                                            gtk_text_iter_get_offset(&start),
                                            gtk_text_iter_get_offset(&end));
    }

    /* Which leaves the cursor where the author was about to type: it was at the
     * end of the line, and a deletion reaching the end of the line brings it
     * back to the head of what went. */
    const gboolean was_applying = editor->applying;
    editor->applying = TRUE;
    gtk_text_buffer_delete(editor->buffer, &start, &end);
    editor->applying = was_applying;

    revalidate(editor, location);
    record_edit(editor, undo_record_new_compound(parts, G_N_ELEMENTS(parts)));

    /* Where a backspace would mean "no, I meant a hyphen". Set last, so the
     * cursor moves this function has just made do not clear it again. */
    editor->autoformat_at = gtk_text_iter_get_offset(location);
}

/* After it lands: dress it. The answer is applied and its opposite removed, so
 * Ctrl+B in the middle of a bold word stops the bold rather than inheriting it
 * from either side. */
static void on_text_inserted(GtkTextBuffer* buffer, GtkTextIter* location,
                             char* text, int length, gpointer user_data)
{
    EditorPanel* editor = user_data;
    if (!editor->inserting) {
        return;
    }
    editor->inserting = FALSE;

    const int      from   = editor->insert_from;
    const int      to     = gtk_text_iter_get_offset(location);
    const uint32_t styles = editor->insert_styles;
    /* Whether the author asked for this, which decides whether text that
     * arrived with styling of its own is overruled. */
    const gboolean asked = editor->asked_mask != 0;
    forget_asked_styles(editor);

    if (to > from && (asked || !range_is_styled(editor, from, to))) {
        for (int bit = 0; bit < INLINE_TAG_COUNT; bit++) {
            GtkTextIter start;
            GtkTextIter end;
            gtk_text_buffer_get_iter_at_offset(buffer, &start, from);
            gtk_text_buffer_get_iter_at_offset(buffer, &end, to);

            if ((styles & (1u << bit)) != 0) {
                gtk_text_buffer_apply_tag(buffer, editor->inline_tags[bit], &start,
                                          &end);
            } else {
                gtk_text_buffer_remove_tag(buffer, editor->inline_tags[bit], &start,
                                           &end);
            }
        }
    }

    dress_block(editor, from, to);

    /* After the styling, not before: the record carries what the text ended up
     * wearing, so redoing an insertion puts back bold text as bold. */
    UndoRecord* parts[2] = { NULL, NULL };
    if (to > from) {
        parts[0] = undo_record_capture_text(buffer, editor->inline_tags,
                                            INLINE_TAG_COUNT, UNDO_TEXT_INSERT,
                                            from, to);
    }

    /* And the item a return opens inside a list, which is part of the same
     * keystroke: one press in, one press to take it back. The text record is
     * captured first because that is the order the two have to be applied in. */
    parts[1] = continue_list(editor, location, to);

    record_edit(editor, undo_record_new_compound(parts, G_N_ELEMENTS(parts)));

    /* And after that record rather than inside it: a marker typed into a list is
     * a second thing done, and the press that takes it back has to leave the
     * characters that asked for it. */
    autoformat_list(editor, location, text, length);

    notify_format(editor);
}

void editor_panel_toggle_style(EditorPanel* editor, uint32_t span_flag)
{
    const int index = inline_tag_index(span_flag);
    if (index < 0 || editor->path == NULL) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    if (gtk_text_buffer_get_selection_bounds(editor->buffer, &start, &end)) {
        GtkTextTag* tag = editor->inline_tags[index];

        /* Toggle off only when the whole selection already carries the tag;
         * otherwise a mixed selection becomes uniformly styled. */
        const gboolean covered = tag_covers(&start, &end, tag);

        /* Captured before the press lands, because the press is what destroys
         * the answer: making a mixed selection uniform is not reversible by
         * pressing again, and only this record will still know the mix. */
        UndoRecord* captured =
            recording(editor)
                ? undo_record_capture_style(editor->buffer, tag, index,
                                            gtk_text_iter_get_offset(&start),
                                            gtk_text_iter_get_offset(&end), !covered)
                : NULL;

        if (covered) {
            gtk_text_buffer_remove_tag(editor->buffer, tag, &start, &end);
        } else {
            gtk_text_buffer_apply_tag(editor->buffer, tag, &start, &end);
        }
        note_style_change(editor);
        record_edit(editor, captured);
        /* The selection has its answer written into it; nothing is left over
         * for the next thing typed. */
        forget_asked_styles(editor);
    } else {
        /* Nothing to style yet, so the answer waits for the text: turn it on
         * against what is in force here, and the next characters typed come out
         * wearing it. */
        editor_ask_for_style(span_flag, editor_panel_styles_at_cursor(editor),
                             &editor->asked_mask, &editor->asked_flags);
    }

    notify_format(editor);
}

/* ── editing verbs ───────────────────────────────────────────────────────── */

/* One record of any kind but a compound. Split out so the compound arm has
 * something to call for each of its parts. */
static void apply_single_record(EditorPanel* editor, const UndoRecord* record,
                                gboolean reverse)
{
    if (record->kind != UNDO_BLOCK) {
        undo_record_apply(record, editor->buffer, editor->inline_tags,
                          INLINE_TAG_COUNT, reverse);
        return;
    }

    apply_block_lines(editor, record->block.lines, record->block.line_count,
                      reverse);

    /* Land the cursor on the change, so a press whose effect is off screen is
     * not one the author has to go looking for. */
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_line(editor->buffer, &at,
                                     record->block.lines[0].line);
    gtk_text_buffer_place_cursor(editor->buffer, &at);
}

/* Whether putting this record back changes a tag without moving a character,
 * which is the case GtkTextBuffer does not call itself modified for. */
static gboolean record_changes_tags(const UndoRecord* record)
{
    if (record->kind == UNDO_STYLE || record->kind == UNDO_BLOCK) {
        return TRUE;
    }
    if (record->kind == UNDO_COMPOUND) {
        for (size_t index = 0; index < record->compound.part_count; index++) {
            if (record_changes_tags(record->compound.parts[index])) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

void editor_panel_apply_record(EditorPanel* editor, const UndoRecord* record,
                               gboolean reverse)
{
    if (editor == NULL || record == NULL || editor->path == NULL) {
        return;
    }

    editor->applying = TRUE;
    if (record->kind == UNDO_COMPOUND) {
        /* In order forwards and in reverse order backwards, which is the whole
         * of what makes a compound different from its parts: taking back an
         * Enter inside a list has to lift the item off before the newline goes,
         * or the marker is left behind as ordinary text. */
        const size_t count = record->compound.part_count;
        for (size_t step = 0; step < count; step++) {
            const size_t index = reverse ? count - 1 - step : step;
            apply_single_record(editor, record->compound.parts[index], reverse);
        }
    } else {
        apply_single_record(editor, record, reverse);
    }
    editor->applying = FALSE;

    if (record_changes_tags(record)) {
        note_style_change(editor);
    }

    forget_asked_styles(editor);
    notify_format(editor);
}

void editor_panel_cut(EditorPanel* editor)
{
    gtk_text_buffer_cut_clipboard(editor->buffer,
                                  gtk_widget_get_clipboard(GTK_WIDGET(editor->view)),
                                  gtk_text_view_get_editable(editor->view));
}

void editor_panel_copy(EditorPanel* editor)
{
    gtk_text_buffer_copy_clipboard(editor->buffer,
                                   gtk_widget_get_clipboard(GTK_WIDGET(editor->view)));
}

void editor_panel_paste(EditorPanel* editor)
{
    gtk_text_buffer_paste_clipboard(editor->buffer,
                                    gtk_widget_get_clipboard(GTK_WIDGET(editor->view)),
                                    NULL,
                                    gtk_text_view_get_editable(editor->view));
}

/* ── composition mode ────────────────────────────────────────────────────── */

int editor_composition_margin(int width, int column)
{
    const int margin = (width - column) / 2;
    return margin > EDITOR_SIDE_MARGIN ? margin : EDITOR_SIDE_MARGIN;
}

static void apply_margins(EditorPanel* editor)
{
    const int margin =
        editor->composing
            ? editor_composition_margin(gtk_widget_get_width(GTK_WIDGET(editor->view)),
                                        EDITOR_COMPOSITION_COLUMN)
            : EDITOR_SIDE_MARGIN;

    gtk_text_view_set_left_margin(editor->view, margin);
    gtk_text_view_set_right_margin(editor->view, margin);
}

/* The scroller's horizontal adjustment reports the visible width, and changes
 * to it are the only notification a plain GtkTextView gets that it has been
 * resized — GTK4 has no size-allocate signal, and subclassing the view to
 * override the vfunc would buy nothing else. The page size is 0 before the
 * first allocation, which editor_composition_margin() reads as too narrow for
 * a column and answers with the ordinary margin. */
static void on_visible_width_changed(GObject* adjustment, GParamSpec* spec,
                                     gpointer user_data)
{
    (void) adjustment;
    (void) spec;

    EditorPanel* editor = user_data;
    if (editor->composing) {
        apply_margins(editor);
    }
}

void editor_panel_set_composition(EditorPanel* editor, gboolean composing)
{
    if (editor == NULL || editor->composing == composing) {
        return;
    }
    editor->composing = composing;
    apply_margins(editor);
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

static void on_buffer_modified(GtkTextBuffer* buffer, gpointer user_data)
{
    EditorPanel* editor = user_data;
    if (editor->loading || editor->modified_callback == NULL) {
        return;
    }
    editor->modified_callback(gtk_text_buffer_get_modified(buffer) ? 1 : 0,
                              editor->modified_user_data);
}

EditorPanel* editor_panel_new(void)
{
    EditorPanel* editor = g_new0(EditorPanel, 1);
    forget_autoformat(editor);   /* -1, which g_new0 does not give */

    GtkWidget* view = gtk_text_view_new();
    editor->view   = GTK_TEXT_VIEW(view);
    editor->buffer = gtk_text_view_get_buffer(editor->view);

    gtk_text_view_set_wrap_mode(editor->view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(editor->view, EDITOR_SIDE_MARGIN);
    gtk_text_view_set_right_margin(editor->view, EDITOR_SIDE_MARGIN);
    gtk_text_view_set_top_margin(editor->view, 32);
    gtk_text_view_set_bottom_margin(editor->view, 32);
    gtk_text_view_set_pixels_below_lines(editor->view, 8);
    /* Nothing is open yet, so there is nowhere for typing to go. */
    gtk_widget_set_sensitive(view, FALSE);

    /* Wordsmith keeps its own history. GtkTextBuffer's records text and not
     * tags, and dies at every set_text(), so leaving it on would only give the
     * buffer a second, shorter opinion about what Ctrl+Z means. */
    gtk_text_buffer_set_enable_undo(editor->buffer, FALSE);

    build_tags(editor);

    g_signal_connect(editor->buffer, "modified-changed",
                     G_CALLBACK(on_buffer_modified), editor);
    g_signal_connect(editor->buffer, "notify::cursor-position",
                     G_CALLBACK(on_cursor_moved), editor);
    /* Both ends of the same insertion: one to read the place before the text
     * covers it, one to style the text once it is there. */
    g_signal_connect(editor->buffer, "insert-text",
                     G_CALLBACK(on_text_inserting), editor);
    g_signal_connect_after(editor->buffer, "insert-text",
                           G_CALLBACK(on_text_inserted), editor);
    /* And both ends of a deletion, to read the text before it goes. */
    g_signal_connect(editor->buffer, "delete-range",
                     G_CALLBACK(on_range_deleting), editor);
    g_signal_connect_after(editor->buffer, "delete-range",
                           G_CALLBACK(on_range_deleted), editor);

    /* On the view rather than the buffer: a marker is tagged non-editable, so a
     * backspace against one never reaches the buffer as a deletion at all.
     * "backspace" is the key binding itself, which is the only place the press
     * can still be seen. */
    g_signal_connect(editor->view, "backspace", G_CALLBACK(on_backspace), editor);

    /* After the panel's own handlers, deliberately: a decoration should read
     * the text as the panel has finished leaving it. Text typed into a code
     * span is not wearing the code tag until on_text_inserted() has put it
     * there, and checking first would mark a word the next line to be looked at
     * would have skipped. */
    editor->spelling = spell_check_new(editor->buffer);
    spell_check_skip_tag(editor->spelling,
                         editor->inline_tags[inline_tag_index(
                             WORDSMITH_MARKUP_SPAN_CODE)]);
    spell_check_skip_tag(editor->spelling, editor->code_block_tag);
    /* And the corrections, which are offered over the view rather than the
     * buffer: a red line is a question, and the answer has to be reachable from
     * the word it is under. */
    spell_check_attach_menu(editor->spelling, editor->view);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "editor-pane");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);

    g_signal_connect(
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller)),
        "notify::page-size", G_CALLBACK(on_visible_width_changed), editor);

    editor->root = scroller;
    return editor;
}

void editor_panel_free(EditorPanel* editor)
{
    if (editor == NULL) {
        return;
    }
    /* Before the buffer goes: the marker has handlers on it to take off. */
    spell_check_free(editor->spelling);
    undo_record_free(editor->pending_delete);
    g_free(editor->path);
    g_free(editor->prologue);
    g_free(editor);
}

GtkWidget* editor_panel_widget(EditorPanel* editor)
{
    return editor != NULL ? editor->root : NULL;
}

void editor_panel_set_modified_callback(EditorPanel* editor,
                                        EditorModifiedFn callback,
                                        void* user_data)
{
    editor->modified_callback  = callback;
    editor->modified_user_data = user_data;
}

void editor_panel_set_styles_callback(EditorPanel* editor, EditorStylesFn callback,
                                      void* user_data)
{
    editor->styles_callback  = callback;
    editor->styles_user_data = user_data;
}

void editor_panel_set_block_callback(EditorPanel* editor, EditorBlockFn callback,
                                     void* user_data)
{
    editor->block_callback  = callback;
    editor->block_user_data = user_data;
}

void editor_panel_set_undo_store(EditorPanel* editor, UndoStore* store)
{
    editor->undo = store;
}

void editor_panel_set_spell_check(EditorPanel* editor, gboolean enabled)
{
    if (editor != NULL) {
        spell_check_set_enabled(editor->spelling, enabled);
    }
}

void editor_panel_set_history_callback(EditorPanel* editor,
                                       EditorHistoryFn callback, void* user_data)
{
    editor->history_callback  = callback;
    editor->history_user_data = user_data;
}

void editor_panel_close(EditorPanel* editor)
{
    note_leaving(editor);
    forget_autoformat(editor);

    editor->loading = TRUE;
    gtk_text_buffer_set_text(editor->buffer, "", 0);
    gtk_text_buffer_set_modified(editor->buffer, FALSE);
    editor->loading = FALSE;

    g_clear_pointer(&editor->path, g_free);
    g_clear_pointer(&editor->prologue, g_free);
    gtk_widget_set_sensitive(GTK_WIDGET(editor->view), FALSE);
    forget_asked_styles(editor);
    notify_format(editor);
}

const char* editor_panel_path(EditorPanel* editor)
{
    return editor != NULL ? editor->path : NULL;
}

gboolean editor_panel_is_modified(EditorPanel* editor)
{
    return editor != NULL && editor->path != NULL
        && gtk_text_buffer_get_modified(editor->buffer);
}
