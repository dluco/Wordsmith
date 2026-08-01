#pragma once

#include <gtk/gtk.h>

#include <stddef.h>
#include <stdint.h>

/* Taking back what was just done.
 *
 * Wordsmith owns its undo history rather than borrowing GtkTextBuffer's. The
 * built-in stack records text insertions and deletions and nothing else, which
 * left two holes an author falls into within a minute of working: Ctrl+B was
 * not undoable at all, because tags are not text, and every history died at
 * `gtk_text_buffer_set_text()` in editor_panel_load(), so switching documents
 * and coming back found nothing to take back.
 *
 * What replaces it is one chronological history per binder item, holding text
 * edits, formatting and metadata edits interleaved in the order they were made.
 *
 * ## One history per item, addressed by the selection
 *
 * Not one stack for the window. Ctrl+Z only ever touches the item the author is
 * looking at, so undo can never yank the view to a document they had stopped
 * thinking about, and switching away and back finds the history where it was
 * left. The history a press addresses is the current binder selection's: a
 * document's holds its text, its formatting and its metadata; a folder's holds
 * only metadata, which is all a folder can have.
 *
 * Histories are session-scoped, in memory, cleared when the project closes.
 * Nothing here is written down.
 *
 * ## Why the records carry more than they seem to need
 *
 * A text record carries the *styling* over its own characters, not just the
 * characters. Undoing an insertion is only a deletion, but redoing one has to
 * put back text that may have been bold, and undoing a deletion has to restore
 * what the deleted run was wearing. Dropping the runs would quietly launder
 * formatting out of the manuscript one undo at a time.
 *
 * A style record carries the tag's *prior coverage* over the range, not a
 * boolean. Bold over a half-bold selection is not reversible by pressing bold
 * again: editor_panel_toggle_style() makes a mixed selection uniform, so the
 * information about which parts were already bold exists only before the press.
 * The record keeps those subranges, and undo puts exactly them back.
 *
 * ## The fingerprint
 *
 * A history outlives the buffer that made it, and its offsets only mean
 * anything against the same text. Switching documents saves, and
 * editor_panel_save() is a lossy round trip through markup.hpp — wrapped
 * paragraphs fold into one line, list markers normalise — so the text that
 * comes back from a later load is not always the text the records were made
 * against.
 *
 * So a history remembers a fingerprint of the buffer as it was left, and a
 * history whose fingerprint does not match the buffer on return is **dropped
 * rather than replayed**. That is session.hpp's contract again: what was
 * written down can only restore what is actually there. In the ordinary case —
 * markup already at its fixed point, which it reaches after one save — the
 * fingerprint matches and the history survives the round trip, which is the
 * whole point of keeping it.
 *
 * ## What is not here yet
 *
 * File operations: create, rename, move, drag-reorder, group, and the delete
 * that does not exist as a command yet. Adding one is a new UndoKind and a new
 * arm in undo_record_apply(), not a redesign — the store is already keyed by
 * binder path. What holds them back is the question they raise and text edits do
 * not: what undo should do when the file has changed on disk since the record
 * was made.
 *
 * Renaming has a second question behind that one. A history is keyed by path, so
 * the one belonging to a renamed document is left under a name nothing selects
 * any more — the same orphaning a move causes, and harmless for the same reason:
 * it is unreachable, it is bounded by the renames in one sitting, and the
 * project closing clears it. Carrying a history across is the fix, and it is the
 * move's to make as much as the rename's. */

/* How many records one item keeps before the oldest is dropped. A day of typing
 * must not grow without a bound, for the reason SESSION_PROJECT_LIMIT exists;
 * a record is a word or so, which puts the ceiling far beyond a sitting. */
#define UNDO_HISTORY_LIMIT 512

typedef enum UndoKind {
    UNDO_TEXT_INSERT,   /* text arrived at [from, from + length) */
    UNDO_TEXT_DELETE,   /* text left from [from, from + length) */
    UNDO_STYLE,         /* one inline tag applied or removed over a range */
    UNDO_METADATA,      /* one field set in frontmatter or a folder's sidecar */
} UndoKind;

/* A run of characters wearing one inline style. Offsets are relative to the
 * start of the record's own text, not to the buffer, so a record stays true
 * wherever the text is put back. */
typedef struct UndoStyleRun {
    int from;
    int to;
    int tag_index;   /* indexes the caller's inline_tags, in bit order */
} UndoStyleRun;

/* A field's value: a scalar, a sequence, or absent.
 *
 * All three are distinct and all three have to survive a round trip, because
 * InspectorEdit already distinguishes them — an author who empties a field is
 * removing it, not setting it to the empty string, and undo has to be able to
 * put the difference back. `scalar` and `items` are both NULL when the field
 * was not there at all. */
typedef struct UndoValue {
    char*  scalar;   /* owned */
    char** items;    /* owned, NULL-terminated */
} UndoValue;

typedef struct UndoText {
    int           from;
    char*         text;        /* owned */
    UndoStyleRun* runs;        /* owned */
    size_t        run_count;
} UndoText;

typedef struct UndoStyle {
    int           from;
    int           to;
    int           tag_index;
    gboolean      applied;     /* whether the press turned the style on */
    UndoStyleRun* prior;       /* what carried the tag before; owned */
    size_t        prior_count;
} UndoStyle;

typedef struct UndoMetadata {
    /* The binder item the field belongs to, not the file the bytes land in: a
     * folder's fields live in a sidecar inside it, and keying by the item is
     * what puts them in the history of the row the author selected. Owned. */
    char*     target;
    gboolean  is_folder;
    char*     key;         /* owned */
    UndoValue before;
    UndoValue after;
} UndoMetadata;

typedef struct UndoRecord {
    UndoKind kind;
    union {
        UndoText     text;       /* UNDO_TEXT_INSERT and UNDO_TEXT_DELETE */
        UndoStyle    style;
        UndoMetadata metadata;
    };
} UndoRecord;

void undo_record_free(UndoRecord* record);

/** What Edit ▸ Undo should call this: "Typing", "Bold", "Synopsis". Caller frees
 *  with g_free(); a metadata record names its own field, so the answer cannot
 *  come from a fixed list without going stale the first time the inspector
 *  learns a new key. The menu names what a press will take back rather than
 *  saying only "Undo", so the author knows what they are about to lose. */
char* undo_record_verb(const UndoRecord* record);

/* ── building records from a buffer ──────────────────────────────────────── */

/** Capture [from, to) as a text record of `kind`, keeping both the characters
 *  and the inline styling over them. `inline_tags[n]` is the tag for `1 << n`,
 *  the order the WORDSMITH_MARKUP_SPAN_* flags are declared in.
 *
 *  For a deletion this must run *before* the text goes, which is why the editor
 *  connects both ends of "delete-range". */
UndoRecord* undo_record_capture_text(GtkTextBuffer* buffer,
                                     GtkTextTag* const* inline_tags, int tag_count,
                                     UndoKind kind, int from, int to);

/** Capture a pending toggle of one tag over [from, to), recording what that tag
 *  already covered inside the range. Must run before the toggle is applied. */
UndoRecord* undo_record_capture_style(GtkTextBuffer* buffer, GtkTextTag* tag,
                                      int tag_index, int from, int to,
                                      gboolean applied);

/** A metadata record. `target` is the binder item, not the file. Takes ownership
 *  of nothing; every string is copied, and both item vectors are
 *  NULL-terminated. A NULL `scalar` with NULL `items` means the field was, or
 *  becomes, absent. */
UndoRecord* undo_record_new_metadata(const char* target, gboolean is_folder,
                                     const char* key,
                                     const char* before_scalar,
                                     const char* const* before_items,
                                     const char* after_scalar,
                                     const char* const* after_items);

/** Put `record` into or out of `buffer`. `reverse` is the undo direction: it
 *  removes an insertion, restores a deletion, and puts a tag's prior coverage
 *  back. UNDO_METADATA touches no buffer and is ignored here — it is applied
 *  through project-actions.c, which knows the ordering a metadata write needs
 *  against the editor. */
void undo_record_apply(const UndoRecord* record, GtkTextBuffer* buffer,
                       GtkTextTag* const* inline_tags, int tag_count,
                       gboolean reverse);

/** A cheap digest of a buffer's text, for the staleness check above. */
guint64 undo_fingerprint(GtkTextBuffer* buffer);

/* ── the coalescing rule ─────────────────────────────────────────────────── */

/** Whether `next` continues `previous` as one run of typing, so that one Ctrl+Z
 *  takes back a word rather than a character.
 *
 *  A run carries on while the text stays contiguous in the direction of travel
 *  and no newline is typed. It breaks where a word does: at the step from
 *  whitespace to a character, so a space joins the word it follows and the
 *  next word starts a fresh entry. Deletions coalesce the same way in both
 *  directions, and backspacing never joins a run of forward deletes — a change
 *  of direction is a change of mind.
 *
 *  Split out from the store so the rule can be checked without a buffer, the
 *  way editor_typed_styles() is. */
gboolean undo_records_coalesce(const UndoRecord* previous, const UndoRecord* next);

/* ── the store ───────────────────────────────────────────────────────────── */

typedef struct UndoStore UndoStore;

UndoStore* undo_store_new(void);
void       undo_store_free(UndoStore* store);

/** Forget every history. The project closing is the whole of "session-scoped". */
void undo_store_clear(UndoStore* store);

/** Push `record` onto `path`'s history, taking ownership of it even when it is
 *  coalesced into the record already on top. Anything that had been undone and
 *  not redone is dropped: a new action is a new future. */
void undo_store_push(UndoStore* store, const char* path, UndoRecord* record);

/** End the current run, so the next record starts a fresh entry. The cursor
 *  moving is what calls this — typing, clicking elsewhere and typing again is
 *  two things done, however close together the offsets happen to fall. */
void undo_store_break_run(UndoStore* store, const char* path);

/** The record a press would take back, or hand forward. Borrowed, and valid
 *  only until the next push. NULL when there is nothing. */
const UndoRecord* undo_store_peek_undo(UndoStore* store, const char* path);
const UndoRecord* undo_store_peek_redo(UndoStore* store, const char* path);

/** Move the cursor over the record that has just been applied. Call after
 *  applying, so a failure to apply leaves the history where it was. */
void undo_store_step_undo(UndoStore* store, const char* path);
void undo_store_step_redo(UndoStore* store, const char* path);

/** Remember what the buffer looked like as `path` was left. */
void undo_store_note_fingerprint(UndoStore* store, const char* path,
                                 guint64 fingerprint);

/** Check a returning document against what was noted, dropping the history when
 *  it no longer describes the text. Safe to call for a path with no history. */
void undo_store_check_fingerprint(UndoStore* store, const char* path,
                                  guint64 fingerprint);

/** Forget one item's history, leaving the rest alone. */
void undo_store_forget(UndoStore* store, const char* path);
