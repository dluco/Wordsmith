#include "editor-panel.h"

#include "spell-check.h"

#include "core/frontmatter-c.h"
#include "core/markup-c.h"
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

    /* Undo. The store is borrowed and outlives the open document, keyed by its
     * path; see undo-stack.h for why a history is per item rather than per
     * window. `applying` is the guard that keeps an undo from being recorded as
     * a fresh edit, in the same idiom as `loading` above. `pending_delete` is
     * a deletion captured before the text went, waiting for it to be gone. */
    UndoStore*  undo;
    UndoRecord* pending_delete;
    gboolean    applying;

    EditorHistoryFn history_callback;
    void*           history_user_data;

    /* The red lines under the misspelled words. Owned, and follows the buffer's
     * own signals from here on; the panel only says when it is loading. */
    SpellCheck* spelling;
};

/* Defined with the rest of the styling, but needed by everything that leaves
 * the cursor somewhere new. */
static void notify_styles(EditorPanel* editor);
static void forget_asked_styles(EditorPanel* editor);

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
    notify_styles(editor);
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

        /* Blank paragraph lines are the author's spacing, not content. The
         * serializer puts a blank line between blocks already. */
        char* raw = gtk_text_buffer_get_text(editor->buffer, &start, &end, FALSE);
        const gboolean blank = raw == NULL || raw[0] == '\0';
        const gboolean is_rule = raw != NULL && strcmp(raw, "---") == 0;
        g_free(raw);

        if (blank && kind.kind == WORDSMITH_MARKUP_PARAGRAPH) {
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

static void notify_styles(EditorPanel* editor)
{
    if (editor->styles_callback == NULL) {
        return;
    }
    editor->styles_callback(editor_panel_styles_at_cursor(editor),
                            editor->styles_user_data);
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
static void on_range_deleting(GtkTextBuffer* buffer, GtkTextIter* start,
                              GtkTextIter* end, gpointer user_data)
{
    EditorPanel* editor = user_data;

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
    }

    forget_asked_styles(editor);
    notify_styles(editor);
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
static void on_text_inserting(GtkTextBuffer* buffer, GtkTextIter* location,
                              char* text, int length, gpointer user_data)
{
    (void) buffer;
    (void) text;
    (void) length;

    EditorPanel* editor = user_data;
    if (editor->loading) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    character_beside(location, &start, &end);

    editor->insert_styles = editor_typed_styles(
        styles_in_range(&start, &end, editor->inline_tags, INLINE_TAG_COUNT),
        editor->asked_mask, editor->asked_flags);
    editor->insert_from = gtk_text_iter_get_offset(location);
    editor->inserting   = TRUE;
}

/* After it lands: dress it. The answer is applied and its opposite removed, so
 * Ctrl+B in the middle of a bold word stops the bold rather than inheriting it
 * from either side. */
static void on_text_inserted(GtkTextBuffer* buffer, GtkTextIter* location,
                             char* text, int length, gpointer user_data)
{
    (void) text;
    (void) length;

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

    /* After the styling, not before: the record carries what the text ended up
     * wearing, so redoing an insertion puts back bold text as bold. */
    if (to > from) {
        record_edit(editor,
                    undo_record_capture_text(buffer, editor->inline_tags,
                                             INLINE_TAG_COUNT, UNDO_TEXT_INSERT,
                                             from, to));
    }

    notify_styles(editor);
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

    notify_styles(editor);
}

/* ── editing verbs ───────────────────────────────────────────────────────── */

void editor_panel_apply_record(EditorPanel* editor, const UndoRecord* record,
                               gboolean reverse)
{
    if (editor == NULL || record == NULL || editor->path == NULL) {
        return;
    }

    editor->applying = TRUE;
    undo_record_apply(record, editor->buffer, editor->inline_tags, INLINE_TAG_COUNT,
                      reverse);
    editor->applying = FALSE;

    if (record->kind == UNDO_STYLE) {
        note_style_change(editor);
    }

    forget_asked_styles(editor);
    notify_styles(editor);
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

    editor->loading = TRUE;
    gtk_text_buffer_set_text(editor->buffer, "", 0);
    gtk_text_buffer_set_modified(editor->buffer, FALSE);
    editor->loading = FALSE;

    g_clear_pointer(&editor->path, g_free);
    g_clear_pointer(&editor->prologue, g_free);
    gtk_widget_set_sensitive(GTK_WIDGET(editor->view), FALSE);
    forget_asked_styles(editor);
    notify_styles(editor);
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
