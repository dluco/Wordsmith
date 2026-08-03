#ifndef WORDSMITH_MARKUP_C_H
#define WORDSMITH_MARKUP_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── opaque handles ─────────────────────────────────────────────────────── */

typedef struct WordsmithMarkupDocument WordsmithMarkupDocument;
typedef struct WordsmithMarkupBuilder  WordsmithMarkupBuilder;

/* ── value types ────────────────────────────────────────────────────────── */

typedef enum {
    WORDSMITH_MARKUP_PARAGRAPH,
    WORDSMITH_MARKUP_HEADING,
    WORDSMITH_MARKUP_CODE_BLOCK,
    WORDSMITH_MARKUP_LIST_ITEM,
    WORDSMITH_MARKUP_QUOTE,
    WORDSMITH_MARKUP_RULE,
} WordsmithMarkupBlockKind;

typedef struct {
    WordsmithMarkupBlockKind kind;
    int                       level;        /* heading level, or list nesting depth */
    int                       ordered;      /* list item: ordered(1) / bullet(0) */
    int                       list_number;  /* ordered list item number */
} WordsmithMarkupBlockInfo;

typedef enum {
    WORDSMITH_MARKUP_SPAN_EMPHASIS  = 1 << 0,
    WORDSMITH_MARKUP_SPAN_STRONG    = 1 << 1,
    WORDSMITH_MARKUP_SPAN_UNDERLINE = 1 << 2,
    WORDSMITH_MARKUP_SPAN_CODE      = 1 << 3,
} WordsmithMarkupSpanFlags;

/* A styled inline run. `text`/`href` are borrowed and valid while the owning
 * document is alive; `href` is NULL when the run is not a link. */
typedef struct {
    const char* text;
    uint32_t    flags;   /* WordsmithMarkupSpanFlags bitset */
    const char* href;
} WordsmithMarkupSpan;

/* ── reading: markdown to a document ────────────────────────────────────── */

/** Parse a Markdown string into a document. Never returns NULL for valid
 *  input (an empty string yields a document with zero blocks). */
WordsmithMarkupDocument* wordsmith_markup_parse(const char* markdown);
void wordsmith_markup_document_free(WordsmithMarkupDocument* doc);

size_t                    wordsmith_markup_block_count(const WordsmithMarkupDocument* doc);
WordsmithMarkupBlockInfo wordsmith_markup_block_info(const WordsmithMarkupDocument* doc,
                                                       size_t block);

/* CODE_BLOCK only: the verbatim body and language (language may be ""). */
const char* wordsmith_markup_block_code(const WordsmithMarkupDocument* doc, size_t block);
const char* wordsmith_markup_block_language(const WordsmithMarkupDocument* doc, size_t block);

/* PARAGRAPH / HEADING / LIST_ITEM / QUOTE: the inline styled spans. */
size_t               wordsmith_markup_block_span_count(const WordsmithMarkupDocument* doc,
                                                        size_t block);
WordsmithMarkupSpan wordsmith_markup_block_span(const WordsmithMarkupDocument* doc,
                                                  size_t block, size_t span);

/* ── writing: a document back to markdown ───────────────────────────────── */

/* The UI walks its text buffer and replays it here, one block at a time, then
 * asks for the Markdown. Escaping and delimiter placement stay in the core so
 * they are testable without a display. */

WordsmithMarkupBuilder* wordsmith_markup_builder_new(void);
void                     wordsmith_markup_builder_free(WordsmithMarkupBuilder* builder);

/** Start a block. `level` is the heading level or list nesting depth, and is
 *  ignored for kinds that have no use for it. Spans added afterwards belong to
 *  this block until the next begin_block call. */
void wordsmith_markup_builder_begin_block(WordsmithMarkupBuilder* builder,
                                           WordsmithMarkupBlockKind kind,
                                           int level);

/** Append a styled run to the current block. `flags` is a
 *  WordsmithMarkupSpanFlags bitset; `href` may be NULL. Text is copied.
 *  Ignored if no block has been started. */
void wordsmith_markup_builder_add_span(WordsmithMarkupBuilder* builder,
                                        const char* text, uint32_t flags,
                                        const char* href);

/** Say whether the current LIST_ITEM is ordered, and what number it carries.
 *
 *  Ordering is a fact about the block, not about its text, and this is the only
 *  way to state it — **the marker belongs to the serializer**. A caller that
 *  writes `- ` or `1. ` into a span of its own gets two markers in the output,
 *  one of them wrong for an ordered item, and gets another on every save after
 *  that.
 *
 *  `number` is ignored for a bullet, and a number below 1 counts as 1. Ignored
 *  when no block has been started and when the current block is not a list
 *  item; the same shape as set_code() below, which is the other fact only one
 *  kind of block has. */
void wordsmith_markup_builder_set_list(WordsmithMarkupBuilder* builder, int ordered,
                                        int number);

/** Set the verbatim body of the current CODE_BLOCK. `language` may be NULL.
 *  Code blocks carry raw text rather than spans, so this replaces add_span for
 *  that kind. Ignored if no block has been started. */
void wordsmith_markup_builder_set_code(WordsmithMarkupBuilder* builder,
                                        const char* code, const char* language);

/** Render everything added so far. Caller owns the result and frees it with
 *  wordsmith_free_string(). Never returns NULL. */
char* wordsmith_markup_builder_to_markdown(const WordsmithMarkupBuilder* builder);

/** Free a string returned by this header. */
void wordsmith_free_string(char* text);

#ifdef __cplusplus
}
#endif

#endif /* WORDSMITH_MARKUP_C_H */
