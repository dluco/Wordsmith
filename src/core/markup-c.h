#ifndef WORDSWORTH_MARKUP_C_H
#define WORDSWORTH_MARKUP_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── opaque handles ─────────────────────────────────────────────────────── */

typedef struct WordsworthMarkupDocument WordsworthMarkupDocument;
typedef struct WordsworthMarkupBuilder  WordsworthMarkupBuilder;

/* ── value types ────────────────────────────────────────────────────────── */

typedef enum {
    WORDSWORTH_MARKUP_PARAGRAPH,
    WORDSWORTH_MARKUP_HEADING,
    WORDSWORTH_MARKUP_CODE_BLOCK,
    WORDSWORTH_MARKUP_LIST_ITEM,
    WORDSWORTH_MARKUP_QUOTE,
    WORDSWORTH_MARKUP_RULE,
} WordsworthMarkupBlockKind;

typedef struct {
    WordsworthMarkupBlockKind kind;
    int                       level;        /* heading level, or list nesting depth */
    int                       ordered;      /* list item: ordered(1) / bullet(0) */
    int                       list_number;  /* ordered list item number */
} WordsworthMarkupBlockInfo;

typedef enum {
    WORDSWORTH_MARKUP_SPAN_EMPHASIS  = 1 << 0,
    WORDSWORTH_MARKUP_SPAN_STRONG    = 1 << 1,
    WORDSWORTH_MARKUP_SPAN_UNDERLINE = 1 << 2,
    WORDSWORTH_MARKUP_SPAN_CODE      = 1 << 3,
} WordsworthMarkupSpanFlags;

/* A styled inline run. `text`/`href` are borrowed and valid while the owning
 * document is alive; `href` is NULL when the run is not a link. */
typedef struct {
    const char* text;
    uint32_t    flags;   /* WordsworthMarkupSpanFlags bitset */
    const char* href;
} WordsworthMarkupSpan;

/* ── reading: markdown to a document ────────────────────────────────────── */

/** Parse a Markdown string into a document. Never returns NULL for valid
 *  input (an empty string yields a document with zero blocks). */
WordsworthMarkupDocument* wordsworth_markup_parse(const char* markdown);
void wordsworth_markup_document_free(WordsworthMarkupDocument* doc);

size_t                    wordsworth_markup_block_count(const WordsworthMarkupDocument* doc);
WordsworthMarkupBlockInfo wordsworth_markup_block_info(const WordsworthMarkupDocument* doc,
                                                       size_t block);

/* CODE_BLOCK only: the verbatim body and language (language may be ""). */
const char* wordsworth_markup_block_code(const WordsworthMarkupDocument* doc, size_t block);
const char* wordsworth_markup_block_language(const WordsworthMarkupDocument* doc, size_t block);

/* PARAGRAPH / HEADING / LIST_ITEM / QUOTE: the inline styled spans. */
size_t               wordsworth_markup_block_span_count(const WordsworthMarkupDocument* doc,
                                                        size_t block);
WordsworthMarkupSpan wordsworth_markup_block_span(const WordsworthMarkupDocument* doc,
                                                  size_t block, size_t span);

/* ── writing: a document back to markdown ───────────────────────────────── */

/* The UI walks its text buffer and replays it here, one block at a time, then
 * asks for the Markdown. Escaping and delimiter placement stay in the core so
 * they are testable without a display. */

WordsworthMarkupBuilder* wordsworth_markup_builder_new(void);
void                     wordsworth_markup_builder_free(WordsworthMarkupBuilder* builder);

/** Start a block. `level` is the heading level or list nesting depth, and is
 *  ignored for kinds that have no use for it. Spans added afterwards belong to
 *  this block until the next begin_block call. */
void wordsworth_markup_builder_begin_block(WordsworthMarkupBuilder* builder,
                                           WordsworthMarkupBlockKind kind,
                                           int level);

/** Append a styled run to the current block. `flags` is a
 *  WordsworthMarkupSpanFlags bitset; `href` may be NULL. Text is copied.
 *  Ignored if no block has been started. */
void wordsworth_markup_builder_add_span(WordsworthMarkupBuilder* builder,
                                        const char* text, uint32_t flags,
                                        const char* href);

/** Set the verbatim body of the current CODE_BLOCK. `language` may be NULL.
 *  Code blocks carry raw text rather than spans, so this replaces add_span for
 *  that kind. Ignored if no block has been started. */
void wordsworth_markup_builder_set_code(WordsworthMarkupBuilder* builder,
                                        const char* code, const char* language);

/** Render everything added so far. Caller owns the result and frees it with
 *  wordsworth_free_string(). Never returns NULL. */
char* wordsworth_markup_builder_to_markdown(const WordsworthMarkupBuilder* builder);

/** Free a string returned by this header. */
void wordsworth_free_string(char* text);

#ifdef __cplusplus
}
#endif

#endif /* WORDSWORTH_MARKUP_C_H */
