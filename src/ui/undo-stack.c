#include "undo-stack.h"

#include <string.h>

/* Offsets here are character offsets, the units GtkTextIter counts in, so every
 * length taken from a record's own text goes through g_utf8_strlen(). Using
 * strlen() would work until the first accented character and then quietly put
 * text back in the wrong place. */

/* ── values ──────────────────────────────────────────────────────────────── */

static void undo_value_set(UndoValue* value, const char* scalar,
                           const char* const* items)
{
    value->scalar = g_strdup(scalar);
    value->items  = items != NULL ? g_strdupv((char**) items) : NULL;
}

static void undo_value_clear(UndoValue* value)
{
    g_clear_pointer(&value->scalar, g_free);
    g_clear_pointer(&value->items, g_strfreev);
}

/* ── records ─────────────────────────────────────────────────────────────── */

void undo_record_free(UndoRecord* record)
{
    if (record == NULL) {
        return;
    }

    switch (record->kind) {
    case UNDO_TEXT_INSERT:
    case UNDO_TEXT_DELETE:
        g_free(record->text.text);
        g_free(record->text.runs);
        break;
    case UNDO_STYLE:
        g_free(record->style.prior);
        break;
    case UNDO_BLOCK:
        g_free(record->block.lines);
        g_free(record->block.verb);
        break;
    case UNDO_COMPOUND:
        for (size_t index = 0; index < record->compound.part_count; index++) {
            undo_record_free(record->compound.parts[index]);
        }
        g_free(record->compound.parts);
        break;
    case UNDO_METADATA:
        g_free(record->metadata.target);
        g_free(record->metadata.key);
        undo_value_clear(&record->metadata.before);
        undo_value_clear(&record->metadata.after);
        break;
    }

    g_free(record);
}

/* The inline tags in bit order, which is the order the WORDSMITH_MARKUP_SPAN_*
 * flags are declared in. Named here rather than derived from the core's enum
 * because what the menu says is a word, not a flag. */
static const char* const STYLE_VERBS[] = { "Italic", "Bold", "Underline", "Code" };

static char* capitalized(const char* key)
{
    if (key == NULL || *key == '\0') {
        return g_strdup("Metadata");
    }

    char head[8];
    const int length = g_unichar_to_utf8(g_unichar_toupper(g_utf8_get_char(key)),
                                         head);
    head[length] = '\0';
    return g_strconcat(head, g_utf8_next_char(key), NULL);
}

char* undo_record_verb(const UndoRecord* record)
{
    if (record == NULL) {
        return NULL;
    }

    switch (record->kind) {
    case UNDO_TEXT_INSERT:
    case UNDO_TEXT_DELETE:
        return g_strdup("Typing");
    case UNDO_STYLE:
        if (record->style.tag_index >= 0
            && record->style.tag_index < (int) G_N_ELEMENTS(STYLE_VERBS)) {
            return g_strdup(STYLE_VERBS[record->style.tag_index]);
        }
        return g_strdup("Formatting");
    case UNDO_BLOCK:
        /* Carried on the record rather than looked up: the block style names
         * are the editor panel's, and this file has no business knowing what a
         * heading is. */
        return g_strdup(record->block.verb != NULL ? record->block.verb
                                                   : "Formatting");
    case UNDO_COMPOUND:
        /* Named for the part that was the author's actual gesture, which is the
         * one they made first — Enter inside a list is Typing, and the item it
         * opens is what typing there means. */
        return record->compound.part_count > 0
                   ? undo_record_verb(record->compound.parts[0])
                   : g_strdup("Formatting");
    case UNDO_METADATA:
        return capitalized(record->metadata.key);
    }

    return NULL;
}

/* ── reading styling out of a buffer ─────────────────────────────────────── */

/* Every run of `tag` inside [from, to), appended to `runs` with offsets made
 * relative to `origin`.
 *
 * Walks the tag's toggles rather than the characters: this runs on every
 * deletion, and a select-all delete would otherwise step through the whole
 * manuscript four times over. */
static void collect_runs(GtkTextBuffer* buffer, GtkTextTag* tag, int tag_index,
                         int from, int to, int origin, GArray* runs)
{
    if (tag == NULL || to <= from) {
        return;
    }

    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(buffer, &iter, from);

    gboolean inside    = gtk_text_iter_has_tag(&iter, tag);
    int      run_start = from;

    while (gtk_text_iter_forward_to_tag_toggle(&iter, tag)) {
        const int at = gtk_text_iter_get_offset(&iter);
        if (at >= to) {
            break;
        }
        if (inside) {
            const UndoStyleRun run = { run_start - origin, at - origin, tag_index };
            g_array_append_val(runs, run);
            inside = FALSE;
        } else {
            run_start = at;
            inside    = TRUE;
        }
    }

    if (inside) {
        const UndoStyleRun run = { run_start - origin, to - origin, tag_index };
        g_array_append_val(runs, run);
    }
}

UndoRecord* undo_record_capture_text(GtkTextBuffer* buffer,
                                     GtkTextTag* const* inline_tags, int tag_count,
                                     UndoKind kind, int from, int to)
{
    if (buffer == NULL || to <= from) {
        return NULL;
    }

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, from);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, to);

    UndoRecord* record = g_new0(UndoRecord, 1);
    record->kind       = kind;
    record->text.from  = from;
    record->text.text  = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);

    GArray* runs = g_array_new(FALSE, FALSE, sizeof(UndoStyleRun));
    for (int bit = 0; bit < tag_count; bit++) {
        collect_runs(buffer, inline_tags != NULL ? inline_tags[bit] : NULL, bit,
                     from, to, from, runs);
    }
    record->text.run_count = runs->len;
    record->text.runs      = (UndoStyleRun*) g_array_free(runs, runs->len == 0);

    return record;
}

UndoRecord* undo_record_capture_style(GtkTextBuffer* buffer, GtkTextTag* tag,
                                      int tag_index, int from, int to,
                                      gboolean applied)
{
    if (buffer == NULL || to <= from) {
        return NULL;
    }

    UndoRecord* record      = g_new0(UndoRecord, 1);
    record->kind            = UNDO_STYLE;
    record->style.from      = from;
    record->style.to        = to;
    record->style.tag_index = tag_index;
    record->style.applied   = applied;

    /* Relative to the range, so undo puts the mix back wherever the range is. */
    GArray* prior = g_array_new(FALSE, FALSE, sizeof(UndoStyleRun));
    collect_runs(buffer, tag, tag_index, from, to, from, prior);
    record->style.prior_count = prior->len;
    record->style.prior = (UndoStyleRun*) g_array_free(prior, prior->len == 0);

    return record;
}

UndoRecord* undo_record_new_compound(UndoRecord** parts, size_t part_count)
{
    if (parts == NULL) {
        return NULL;
    }

    /* Dropping the NULLs here is what lets a caller hand over whatever it
     * happened to make without deciding first whether it has one thing or two. */
    UndoRecord** kept = g_new0(UndoRecord*, part_count > 0 ? part_count : 1);
    size_t       count = 0;
    for (size_t index = 0; index < part_count; index++) {
        if (parts[index] != NULL) {
            kept[count++] = parts[index];
        }
    }

    if (count == 0) {
        g_free(kept);
        return NULL;
    }
    if (count == 1) {
        /* One thing is not a sequence, and wrapping it would only cost every
         * reader of the history a level to see through. */
        UndoRecord* only = kept[0];
        g_free(kept);
        return only;
    }

    UndoRecord* record         = g_new0(UndoRecord, 1);
    record->kind               = UNDO_COMPOUND;
    record->compound.parts     = kept;
    record->compound.part_count = count;

    return record;
}

UndoRecord* undo_record_new_block(const char* verb, const UndoBlockLine* lines,
                                  size_t line_count)
{
    if (lines == NULL || line_count == 0) {
        return NULL;
    }

    UndoRecord* record      = g_new0(UndoRecord, 1);
    record->kind            = UNDO_BLOCK;
    record->block.verb      = g_strdup(verb);
    record->block.line_count = line_count;
    record->block.lines     = g_memdup2(lines, line_count * sizeof(*lines));

    return record;
}

UndoRecord* undo_record_new_metadata(const char* target, gboolean is_folder,
                                     const char* key,
                                     const char* before_scalar,
                                     const char* const* before_items,
                                     const char* after_scalar,
                                     const char* const* after_items)
{
    if (target == NULL || key == NULL) {
        return NULL;
    }

    UndoRecord* record         = g_new0(UndoRecord, 1);
    record->kind               = UNDO_METADATA;
    record->metadata.target    = g_strdup(target);
    record->metadata.is_folder = is_folder;
    record->metadata.key       = g_strdup(key);
    undo_value_set(&record->metadata.before, before_scalar, before_items);
    undo_value_set(&record->metadata.after, after_scalar, after_items);

    return record;
}

/* ── putting a record into a buffer ──────────────────────────────────────── */

static void clear_inline_tags(GtkTextBuffer* buffer, GtkTextTag* const* inline_tags,
                              int tag_count, int from, int to)
{
    for (int bit = 0; bit < tag_count; bit++) {
        if (inline_tags == NULL || inline_tags[bit] == NULL) {
            continue;
        }
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(buffer, &start, from);
        gtk_text_buffer_get_iter_at_offset(buffer, &end, to);
        gtk_text_buffer_remove_tag(buffer, inline_tags[bit], &start, &end);
    }
}

static void apply_runs(GtkTextBuffer* buffer, GtkTextTag* const* inline_tags,
                       int tag_count, int origin, const UndoStyleRun* runs,
                       size_t count)
{
    for (size_t index = 0; index < count; index++) {
        const UndoStyleRun* run = &runs[index];
        if (inline_tags == NULL || run->tag_index < 0 || run->tag_index >= tag_count
            || inline_tags[run->tag_index] == NULL) {
            continue;
        }
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(buffer, &start, origin + run->from);
        gtk_text_buffer_get_iter_at_offset(buffer, &end, origin + run->to);
        gtk_text_buffer_apply_tag(buffer, inline_tags[run->tag_index], &start, &end);
    }
}

/* Put the text back, wearing exactly what it wore.
 *
 * The tags are cleared before the runs go on because GTK gives inserted text
 * the tags covering the spot it lands in: text put back inside a bold word
 * would come out bold whatever the record says. Clearing first makes the record
 * the only authority on the styling, which is the point of carrying it. */
static void put_text_back(const UndoRecord* record, GtkTextBuffer* buffer,
                          GtkTextTag* const* inline_tags, int tag_count)
{
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer, &at, record->text.from);
    gtk_text_buffer_insert(buffer, &at, record->text.text, -1);

    const int to = record->text.from + (int) g_utf8_strlen(record->text.text, -1);
    clear_inline_tags(buffer, inline_tags, tag_count, record->text.from, to);
    apply_runs(buffer, inline_tags, tag_count, record->text.from, record->text.runs,
               record->text.run_count);

    gtk_text_buffer_get_iter_at_offset(buffer, &at, to);
    gtk_text_buffer_place_cursor(buffer, &at);
}

static void take_text_away(const UndoRecord* record, GtkTextBuffer* buffer)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, record->text.from);
    gtk_text_buffer_get_iter_at_offset(
        buffer, &end,
        record->text.from + (int) g_utf8_strlen(record->text.text, -1));
    gtk_text_buffer_delete(buffer, &start, &end);
    gtk_text_buffer_place_cursor(buffer, &start);
}

void undo_record_apply(const UndoRecord* record, GtkTextBuffer* buffer,
                       GtkTextTag* const* inline_tags, int tag_count,
                       gboolean reverse)
{
    if (record == NULL || buffer == NULL) {
        return;
    }

    switch (record->kind) {
    case UNDO_TEXT_INSERT:
        if (reverse) {
            take_text_away(record, buffer);
        } else {
            put_text_back(record, buffer, inline_tags, tag_count);
        }
        return;

    case UNDO_TEXT_DELETE:
        if (reverse) {
            put_text_back(record, buffer, inline_tags, tag_count);
        } else {
            take_text_away(record, buffer);
        }
        return;

    case UNDO_STYLE: {
        const int index = record->style.tag_index;
        if (inline_tags == NULL || index < 0 || index >= tag_count
            || inline_tags[index] == NULL) {
            return;
        }
        GtkTextTag* tag = inline_tags[index];

        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(buffer, &start, record->style.from);
        gtk_text_buffer_get_iter_at_offset(buffer, &end, record->style.to);

        if (!reverse) {
            if (record->style.applied) {
                gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
            } else {
                gtk_text_buffer_remove_tag(buffer, tag, &start, &end);
            }
            gtk_text_buffer_select_range(buffer, &start, &end);
            return;
        }

        /* Undo restores the coverage the range had before the press, which a
         * second toggle could not: the press made a mixed selection uniform and
         * only the record still knows the mix. */
        gtk_text_buffer_remove_tag(buffer, tag, &start, &end);
        apply_runs(buffer, inline_tags, tag_count, record->style.from,
                   record->style.prior, record->style.prior_count);
        gtk_text_buffer_get_iter_at_offset(buffer, &start, record->style.from);
        gtk_text_buffer_get_iter_at_offset(buffer, &end, record->style.to);
        gtk_text_buffer_select_range(buffer, &start, &end);
        return;
    }

    case UNDO_BLOCK:
        /* Applied through editor_panel_apply_record(), which owns the buffer's
         * block tags and the list markers made out of them. */
        return;

    case UNDO_COMPOUND:
        /* Also editor_panel_apply_record()'s: a compound may hold either kind,
         * and walking it here too would split one sequence across two passes
         * and put the parts in the wrong order. */
        return;

    case UNDO_METADATA:
        /* Applied through project-actions.c, which owns the ordering a metadata
         * write needs against the editor's own copy of the frontmatter. */
        return;
    }
}

guint64 undo_fingerprint(GtkTextBuffer* buffer)
{
    if (buffer == NULL) {
        return 0;
    }

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char* text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);

    /* FNV-1a, which is all this needs: it is comparing a buffer against itself
     * one document switch later, not defending against anything. */
    guint64 hash = 1469598103934665603ULL;
    for (const unsigned char* at = (const unsigned char*) text; *at != '\0'; at++) {
        hash = (hash ^ *at) * 1099511628211ULL;
    }

    g_free(text);
    return hash;
}

/* ── the coalescing rule ─────────────────────────────────────────────────── */

static gunichar first_char(const char* text)
{
    return (text == NULL || *text == '\0') ? 0 : g_utf8_get_char(text);
}

static gunichar last_char(const char* text)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }
    return g_utf8_get_char(g_utf8_prev_char(text + strlen(text)));
}

/* Where a word ends: the step from whitespace to a character. A space therefore
 * joins the word it follows and the next word opens a fresh entry, which is
 * what makes one Ctrl+Z take back "hello " rather than "o". */
static gboolean word_break(gunichar before, gunichar after)
{
    return g_unichar_isspace(before) && !g_unichar_isspace(after);
}

static gboolean is_text_kind(UndoKind kind)
{
    return kind == UNDO_TEXT_INSERT || kind == UNDO_TEXT_DELETE;
}

gboolean undo_records_coalesce(const UndoRecord* previous, const UndoRecord* next)
{
    if (previous == NULL || next == NULL || previous->kind != next->kind
        || !is_text_kind(previous->kind)) {
        return FALSE;
    }

    const char* before = previous->text.text;
    const char* after  = next->text.text;
    if (before == NULL || *before == '\0' || after == NULL || *after == '\0') {
        return FALSE;
    }
    /* A newline is a place the author stopped, whichever side it falls on. */
    if (strchr(before, '\n') != NULL || strchr(after, '\n') != NULL) {
        return FALSE;
    }

    const int before_length = (int) g_utf8_strlen(before, -1);
    const int after_length  = (int) g_utf8_strlen(after, -1);

    if (previous->kind == UNDO_TEXT_INSERT) {
        if (next->text.from != previous->text.from + before_length) {
            return FALSE;
        }
        return !word_break(last_char(before), first_char(after));
    }

    /* Forward delete: the offset stays put as the text comes to meet it. */
    if (next->text.from == previous->text.from) {
        return !word_break(last_char(before), first_char(after));
    }
    /* Backspace: the offset walks left, and the run reads right to left. */
    if (next->text.from + after_length == previous->text.from) {
        return !word_break(last_char(after), first_char(before));
    }
    return FALSE;
}

/* ── merging one record into the one before it ───────────────────────────── */

static void append_runs(UndoText* text, const UndoStyleRun* runs, size_t count,
                        int shift)
{
    if (count == 0) {
        return;
    }

    const size_t total = text->run_count + count;
    text->runs = g_renew(UndoStyleRun, text->runs, total);
    for (size_t index = 0; index < count; index++) {
        UndoStyleRun run = runs[index];
        run.from += shift;
        run.to   += shift;
        text->runs[text->run_count + index] = run;
    }
    text->run_count = total;
}

static void shift_runs(UndoText* text, int shift)
{
    for (size_t index = 0; index < text->run_count; index++) {
        text->runs[index].from += shift;
        text->runs[index].to   += shift;
    }
}

/* `next` has been decided to continue `previous`; fold it in. The three cases
 * are the three the rule above admits, and each moves the styling with the
 * characters it belongs to. */
static void merge_records(UndoRecord* previous, const UndoRecord* next)
{
    const int before_length = (int) g_utf8_strlen(previous->text.text, -1);
    const int after_length  = (int) g_utf8_strlen(next->text.text, -1);

    const gboolean backspacing =
        previous->kind == UNDO_TEXT_DELETE
        && next->text.from + after_length == previous->text.from
        && next->text.from != previous->text.from;

    if (backspacing) {
        char* joined = g_strconcat(next->text.text, previous->text.text, NULL);
        g_free(previous->text.text);
        previous->text.text = joined;

        /* What was already here now sits after the newly deleted characters. */
        shift_runs(&previous->text, after_length);
        append_runs(&previous->text, next->text.runs, next->text.run_count, 0);
        previous->text.from = next->text.from;
        return;
    }

    char* joined = g_strconcat(previous->text.text, next->text.text, NULL);
    g_free(previous->text.text);
    previous->text.text = joined;
    append_runs(&previous->text, next->text.runs, next->text.run_count,
                before_length);
}

/* ── the store ───────────────────────────────────────────────────────────── */

typedef struct UndoHistory {
    GPtrArray* records;          /* UndoRecord*, oldest first */
    guint      cursor;           /* [0, cursor) are done; the rest are undone */
    gboolean   run_open;         /* whether the top record may still grow */
    guint64    fingerprint;
    gboolean   have_fingerprint;
} UndoHistory;

struct UndoStore {
    GHashTable* histories;   /* char* path → UndoHistory* */
};

static void history_free(gpointer data)
{
    UndoHistory* history = data;
    g_ptr_array_free(history->records, TRUE);
    g_free(history);
}

UndoStore* undo_store_new(void)
{
    UndoStore* store = g_new0(UndoStore, 1);
    store->histories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             history_free);
    return store;
}

void undo_store_free(UndoStore* store)
{
    if (store == NULL) {
        return;
    }
    g_hash_table_destroy(store->histories);
    g_free(store);
}

void undo_store_clear(UndoStore* store)
{
    if (store != NULL) {
        g_hash_table_remove_all(store->histories);
    }
}

void undo_store_forget(UndoStore* store, const char* path)
{
    if (store != NULL && path != NULL) {
        g_hash_table_remove(store->histories, path);
    }
}

static UndoHistory* history_for(UndoStore* store, const char* path, gboolean create)
{
    if (store == NULL || path == NULL) {
        return NULL;
    }

    UndoHistory* history = g_hash_table_lookup(store->histories, path);
    if (history != NULL || !create) {
        return history;
    }

    history = g_new0(UndoHistory, 1);
    history->records =
        g_ptr_array_new_with_free_func((GDestroyNotify) undo_record_free);
    g_hash_table_insert(store->histories, g_strdup(path), history);
    return history;
}

void undo_store_push(UndoStore* store, const char* path, UndoRecord* record)
{
    UndoHistory* history = history_for(store, path, TRUE);
    if (history == NULL || record == NULL) {
        undo_record_free(record);
        return;
    }

    /* A new action is a new future: whatever had been undone and not redone is
     * no longer reachable, and holding it would let a press hand back something
     * that never followed from what is now on screen. */
    while (history->records->len > history->cursor) {
        g_ptr_array_remove_index(history->records, history->records->len - 1);
    }

    if (history->run_open && history->cursor > 0) {
        UndoRecord* top = g_ptr_array_index(history->records, history->cursor - 1);
        if (undo_records_coalesce(top, record)) {
            merge_records(top, record);
            undo_record_free(record);
            return;
        }
    }

    g_ptr_array_add(history->records, record);
    history->cursor   = history->records->len;
    history->run_open = TRUE;

    /* Oldest first, so the cursor follows the records it still describes. */
    while (history->records->len > UNDO_HISTORY_LIMIT) {
        g_ptr_array_remove_index(history->records, 0);
        if (history->cursor > 0) {
            history->cursor--;
        }
    }
}

void undo_store_break_run(UndoStore* store, const char* path)
{
    UndoHistory* history = history_for(store, path, FALSE);
    if (history != NULL) {
        history->run_open = FALSE;
    }
}

const UndoRecord* undo_store_peek_undo(UndoStore* store, const char* path)
{
    UndoHistory* history = history_for(store, path, FALSE);
    if (history == NULL || history->cursor == 0) {
        return NULL;
    }
    return g_ptr_array_index(history->records, history->cursor - 1);
}

const UndoRecord* undo_store_peek_redo(UndoStore* store, const char* path)
{
    UndoHistory* history = history_for(store, path, FALSE);
    if (history == NULL || history->cursor >= history->records->len) {
        return NULL;
    }
    return g_ptr_array_index(history->records, history->cursor);
}

void undo_store_step_undo(UndoStore* store, const char* path)
{
    UndoHistory* history = history_for(store, path, FALSE);
    if (history != NULL && history->cursor > 0) {
        history->cursor--;
        /* The run is closed either way: the next thing typed is a new entry
         * rather than a continuation of something that has been taken back. */
        history->run_open = FALSE;
    }
}

void undo_store_step_redo(UndoStore* store, const char* path)
{
    UndoHistory* history = history_for(store, path, FALSE);
    if (history != NULL && history->cursor < history->records->len) {
        history->cursor++;
        history->run_open = FALSE;
    }
}

void undo_store_note_fingerprint(UndoStore* store, const char* path,
                                 guint64 fingerprint)
{
    UndoHistory* history = history_for(store, path, FALSE);
    if (history != NULL) {
        history->fingerprint      = fingerprint;
        history->have_fingerprint = TRUE;
        /* Leaving the document ends the run, whatever the offsets say. */
        history->run_open = FALSE;
    }
}

void undo_store_check_fingerprint(UndoStore* store, const char* path,
                                  guint64 fingerprint)
{
    UndoHistory* history = history_for(store, path, FALSE);
    if (history == NULL) {
        return;
    }

    /* A history whose offsets no longer describe the text is worse than no
     * history: replaying it would put characters back in the wrong places. Save
     * normalises markup, so this is a real outcome and not a defensive check. */
    if (!history->have_fingerprint || history->fingerprint != fingerprint) {
        undo_store_forget(store, path);
    }
}
