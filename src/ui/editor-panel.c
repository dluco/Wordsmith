#include "editor-panel.h"

#include "core/markup-c.h"
#include "core/project-c.h"

#include <string.h>

/* Inline style tags, indexed by bit position in WordsworthMarkupSpanFlags. */
#define INLINE_TAG_COUNT 4

/* Deepest ATX heading Markdown allows. */
#define MAX_HEADING_LEVEL 6

struct EditorPanel {
    GtkWidget*     root;   /* borrowed once parented into the window */
    GtkTextView*   view;
    GtkTextBuffer* buffer;

    char*    path;      /* owned; NULL when nothing is open */
    gboolean loading;   /* suppresses the modified callback during load */

    GtkTextTag* inline_tags[INLINE_TAG_COUNT];
    GtkTextTag* heading_tags[MAX_HEADING_LEVEL];
    GtkTextTag* quote_tag;
    GtkTextTag* code_block_tag;
    GtkTextTag* bullet_tag;
    GtkTextTag* ordered_tag;
    GtkTextTag* marker_tag;   /* list markers: shown, but not part of the text */

    EditorModifiedFn modified_callback;
    void*            modified_user_data;
};

/* ── tags ────────────────────────────────────────────────────────────────── */

/* Bit position of a single-bit span flag, so flags index into inline_tags. */
static int inline_tag_index(uint32_t span_flag)
{
    switch (span_flag) {
    case WORDSWORTH_MARKUP_SPAN_EMPHASIS:  return 0;
    case WORDSWORTH_MARKUP_SPAN_STRONG:    return 1;
    case WORDSWORTH_MARKUP_SPAN_UNDERLINE: return 2;
    case WORDSWORTH_MARKUP_SPAN_CODE:      return 3;
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

static void insert_spans(EditorPanel* editor, const WordsworthMarkupDocument* doc,
                         size_t block)
{
    const size_t span_count = wordsworth_markup_block_span_count(doc, block);
    for (size_t index = 0; index < span_count; index++) {
        WordsworthMarkupSpan span = wordsworth_markup_block_span(doc, block, index);

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

static void insert_block(EditorPanel* editor, const WordsworthMarkupDocument* doc,
                         size_t block)
{
    const WordsworthMarkupBlockInfo info = wordsworth_markup_block_info(doc, block);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(editor->buffer, &end);
    const int start_offset = gtk_text_iter_get_offset(&end);

    switch (info.kind) {
    case WORDSWORTH_MARKUP_HEADING: {
        int level = info.level < 1 ? 1 : info.level;
        if (level > MAX_HEADING_LEVEL) {
            level = MAX_HEADING_LEVEL;
        }
        insert_spans(editor, doc, block);
        apply_to_block(editor, editor->heading_tags[level - 1], start_offset);
        break;
    }

    case WORDSWORTH_MARKUP_QUOTE:
        insert_spans(editor, doc, block);
        apply_to_block(editor, editor->quote_tag, start_offset);
        break;

    case WORDSWORTH_MARKUP_CODE_BLOCK: {
        const char* code = wordsworth_markup_block_code(doc, block);
        gtk_text_buffer_insert(editor->buffer, &end, code, -1);
        apply_to_block(editor, editor->code_block_tag, start_offset);
        break;
    }

    case WORDSWORTH_MARKUP_LIST_ITEM: {
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

    case WORDSWORTH_MARKUP_RULE:
        /* Rules have no spans. The visible text doubles as what save() reads
         * back, since a lone "---" line re-parses as a rule. */
        gtk_text_buffer_insert(editor->buffer, &end, "---", -1);
        break;

    case WORDSWORTH_MARKUP_PARAGRAPH:
        insert_spans(editor, doc, block);
        break;
    }
}

int editor_panel_load(EditorPanel* editor, const char* path, char** error)
{
    char* markdown = wordsworth_document_read(path, error);
    if (markdown == NULL) {
        return 0;
    }

    WordsworthMarkupDocument* doc = wordsworth_markup_parse(markdown);
    wordsworth_free_string(markdown);
    if (doc == NULL) {
        if (error != NULL) {
            *error = g_strdup("out of memory parsing document");
        }
        return 0;
    }

    editor->loading = TRUE;
    gtk_text_buffer_set_text(editor->buffer, "", 0);

    const size_t block_count = wordsworth_markup_block_count(doc);
    for (size_t block = 0; block < block_count; block++) {
        if (block > 0) {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(editor->buffer, &end);
            gtk_text_buffer_insert(editor->buffer, &end, "\n", 1);
        }
        insert_block(editor, doc, block);
    }

    wordsworth_markup_document_free(doc);

    g_free(editor->path);
    editor->path = g_strdup(path);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(editor->buffer, &start);
    gtk_text_buffer_place_cursor(editor->buffer, &start);
    gtk_text_buffer_set_modified(editor->buffer, FALSE);
    editor->loading = FALSE;

    gtk_widget_set_sensitive(GTK_WIDGET(editor->view), TRUE);
    return 1;
}

/* ── saving ──────────────────────────────────────────────────────────────── */

/* Which block kind a line carries, read from the tags at its first character. */
typedef struct LineKind {
    WordsworthMarkupBlockKind kind;
    int                       level;
} LineKind;

static LineKind line_kind(EditorPanel* editor, const GtkTextIter* line_start)
{
    LineKind out = { WORDSWORTH_MARKUP_PARAGRAPH, 0 };

    for (int level = 0; level < MAX_HEADING_LEVEL; level++) {
        if (gtk_text_iter_has_tag(line_start, editor->heading_tags[level])) {
            out.kind  = WORDSWORTH_MARKUP_HEADING;
            out.level = level + 1;
            return out;
        }
    }
    if (gtk_text_iter_has_tag(line_start, editor->code_block_tag)) {
        out.kind = WORDSWORTH_MARKUP_CODE_BLOCK;
        return out;
    }
    if (gtk_text_iter_has_tag(line_start, editor->quote_tag)) {
        out.kind = WORDSWORTH_MARKUP_QUOTE;
        return out;
    }
    if (gtk_text_iter_has_tag(line_start, editor->ordered_tag)) {
        out.kind    = WORDSWORTH_MARKUP_LIST_ITEM;
        out.level   = 1;   /* ordered, recorded in `level` for the caller */
        return out;
    }
    if (gtk_text_iter_has_tag(line_start, editor->bullet_tag)) {
        out.kind  = WORDSWORTH_MARKUP_LIST_ITEM;
        out.level = 0;
        return out;
    }
    return out;
}

/* Emit the spans between `start` and `end`, splitting wherever the set of
 * inline tags changes. Marker text is skipped. */
static void add_spans_for_range(EditorPanel* editor,
                                WordsworthMarkupBuilder* builder,
                                const GtkTextIter* start, const GtkTextIter* end)
{
    GtkTextIter cursor = *start;

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
            char* text = gtk_text_buffer_get_text(editor->buffer, &cursor, &run_end,
                                                  FALSE);
            if (text != NULL && text[0] != '\0') {
                wordsworth_markup_builder_add_span(builder, text, flags, NULL);
            }
            g_free(text);
        }

        cursor = run_end;
    }
}

/* Gather the run of code-block lines starting at `line`, emit it as one block,
 * and return the first line after it. */
static int emit_code_block(EditorPanel* editor, WordsworthMarkupBuilder* builder,
                           int line, int line_count)
{
    GString* code = g_string_new(NULL);
    int current = line;

    while (current < line_count) {
        GtkTextIter start;
        gtk_text_buffer_get_iter_at_line(editor->buffer, &start, current);
        if (line_kind(editor, &start).kind != WORDSWORTH_MARKUP_CODE_BLOCK) {
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

    wordsworth_markup_builder_begin_block(builder, WORDSWORTH_MARKUP_CODE_BLOCK, 0);
    wordsworth_markup_builder_set_code(builder, code->str, NULL);
    g_string_free(code, TRUE);
    return current;
}

int editor_panel_save(EditorPanel* editor, char** error)
{
    if (editor->path == NULL) {
        return 1;
    }

    WordsworthMarkupBuilder* builder = wordsworth_markup_builder_new();
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

        if (kind.kind == WORDSWORTH_MARKUP_CODE_BLOCK) {
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

        if (blank && kind.kind == WORDSWORTH_MARKUP_PARAGRAPH) {
            previous_was_ordered = FALSE;
            line++;
            continue;
        }

        if (is_rule && kind.kind == WORDSWORTH_MARKUP_PARAGRAPH) {
            wordsworth_markup_builder_begin_block(builder, WORDSWORTH_MARKUP_RULE, 0);
            previous_was_ordered = FALSE;
            line++;
            continue;
        }

        if (kind.kind == WORDSWORTH_MARKUP_LIST_ITEM) {
            const gboolean ordered = kind.level == 1;
            if (!ordered || !previous_was_ordered) {
                list_number = 1;
            }
            wordsworth_markup_builder_begin_block(builder,
                                                  WORDSWORTH_MARKUP_LIST_ITEM, 0);
            /* begin_block takes nesting depth in `level`; ordering and the
             * running number are set through the marker we regenerate here. */
            if (ordered) {
                char marker[24];
                g_snprintf(marker, sizeof(marker), "%d. ", list_number);
                list_number++;
                wordsworth_markup_builder_add_span(builder, marker, 0, NULL);
            } else {
                wordsworth_markup_builder_add_span(builder, "- ", 0, NULL);
            }
            previous_was_ordered = ordered;
        } else {
            wordsworth_markup_builder_begin_block(builder, kind.kind, kind.level);
            previous_was_ordered = FALSE;
        }

        add_spans_for_range(editor, builder, &start, &end);
        line++;
    }

    char* markdown = wordsworth_markup_builder_to_markdown(builder);
    wordsworth_markup_builder_free(builder);

    const int ok = wordsworth_document_write(editor->path,
                                             markdown != NULL ? markdown : "", error);
    wordsworth_free_string(markdown);

    if (ok) {
        gtk_text_buffer_set_modified(editor->buffer, FALSE);
    }
    return ok;
}

/* ── styling ─────────────────────────────────────────────────────────────── */

void editor_panel_toggle_style(EditorPanel* editor, uint32_t span_flag)
{
    const int index = inline_tag_index(span_flag);
    if (index < 0) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    if (!gtk_text_buffer_get_selection_bounds(editor->buffer, &start, &end)) {
        return;
    }

    GtkTextTag* tag = editor->inline_tags[index];

    /* Toggle off only when the whole selection already carries the tag;
     * otherwise a mixed selection becomes uniformly styled. */
    gboolean fully_tagged = TRUE;
    GtkTextIter cursor = start;
    while (gtk_text_iter_compare(&cursor, &end) < 0) {
        if (!gtk_text_iter_has_tag(&cursor, tag)) {
            fully_tagged = FALSE;
            break;
        }
        gtk_text_iter_forward_char(&cursor);
    }

    if (fully_tagged) {
        gtk_text_buffer_remove_tag(editor->buffer, tag, &start, &end);
    } else {
        gtk_text_buffer_apply_tag(editor->buffer, tag, &start, &end);
    }
}

/* ── editing verbs ───────────────────────────────────────────────────────── */

void editor_panel_undo(EditorPanel* editor)
{
    if (gtk_text_buffer_get_can_undo(editor->buffer)) {
        gtk_text_buffer_undo(editor->buffer);
    }
}

void editor_panel_redo(EditorPanel* editor)
{
    if (gtk_text_buffer_get_can_redo(editor->buffer)) {
        gtk_text_buffer_redo(editor->buffer);
    }
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
    gtk_text_view_set_left_margin(editor->view, 48);
    gtk_text_view_set_right_margin(editor->view, 48);
    gtk_text_view_set_top_margin(editor->view, 32);
    gtk_text_view_set_bottom_margin(editor->view, 32);
    gtk_text_view_set_pixels_below_lines(editor->view, 8);
    /* Nothing is open yet, so there is nowhere for typing to go. */
    gtk_widget_set_sensitive(view, FALSE);

    build_tags(editor);

    g_signal_connect(editor->buffer, "modified-changed",
                     G_CALLBACK(on_buffer_modified), editor);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "editor-pane");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);

    editor->root = scroller;
    return editor;
}

void editor_panel_free(EditorPanel* editor)
{
    if (editor == NULL) {
        return;
    }
    g_free(editor->path);
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

void editor_panel_close(EditorPanel* editor)
{
    editor->loading = TRUE;
    gtk_text_buffer_set_text(editor->buffer, "", 0);
    gtk_text_buffer_set_modified(editor->buffer, FALSE);
    editor->loading = FALSE;

    g_clear_pointer(&editor->path, g_free);
    gtk_widget_set_sensitive(GTK_WIDGET(editor->view), FALSE);
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
