#ifndef WORDSMITH_FRONTMATTER_C_H
#define WORDSMITH_FRONTMATTER_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A document's parsed frontmatter. Strings handed out here are borrowed and
 * stay valid until the handle is freed. */
typedef struct WordsmithFrontmatter WordsmithFrontmatter;

typedef enum {
    WORDSMITH_FRONTMATTER_SCALAR,
    WORDSMITH_FRONTMATTER_SEQUENCE,
    WORDSMITH_FRONTMATTER_MAP,
    WORDSMITH_FRONTMATTER_MISSING,
} WordsmithFrontmatterValueKind;

/** Split `text` and parse whatever frontmatter it has. Never returns NULL for
 *  valid input; a document with no frontmatter yields a handle reporting zero
 *  keys and a body offset of zero. */
WordsmithFrontmatter* wordsmith_frontmatter_parse(const char* text);

/** Parse `text` as a bare YAML document — a folder's metadata.yaml, which has
 *  no fences to find — and hand it back through the same accessors. Reports
 *  no frontmatter present and a body offset of zero, since it is all
 *  metadata. */
WordsmithFrontmatter* wordsmith_frontmatter_parse_yaml(const char* text);
void                  wordsmith_frontmatter_free(WordsmithFrontmatter* frontmatter);

/** Whether a fenced block was found. Distinct from "has keys": an empty block
 *  is present but carries nothing. */
int wordsmith_frontmatter_present(const WordsmithFrontmatter* frontmatter);

/** Byte offset where the Markdown body starts. The editor loads from here and
 *  keeps everything before it to put back on save. */
size_t wordsmith_frontmatter_body_offset(const WordsmithFrontmatter* frontmatter);

/* Keys in the order the file lists them, so the inspector can show
 * unrecognised fields the way they were written. */
size_t      wordsmith_frontmatter_key_count(const WordsmithFrontmatter* frontmatter);
const char* wordsmith_frontmatter_key_at(const WordsmithFrontmatter* frontmatter,
                                         size_t index);

WordsmithFrontmatterValueKind
wordsmith_frontmatter_value_kind(const WordsmithFrontmatter* frontmatter,
                                 const char* key);

/** Scalar value for `key`, or NULL when the key is absent or holds a
 *  collection. */
const char* wordsmith_frontmatter_string(const WordsmithFrontmatter* frontmatter,
                                         const char* key);

size_t      wordsmith_frontmatter_sequence_count(const WordsmithFrontmatter* frontmatter,
                                                 const char* key);
const char* wordsmith_frontmatter_sequence_at(const WordsmithFrontmatter* frontmatter,
                                              const char* key, size_t index);

/* What the parser could not make sense of. The frontmatter still splits — the
 * body is still the body — so these are worth showing rather than failing on. */
size_t      wordsmith_frontmatter_diagnostic_count(const WordsmithFrontmatter* frontmatter);
const char* wordsmith_frontmatter_diagnostic_at(const WordsmithFrontmatter* frontmatter,
                                                size_t index, size_t* line);

/** Return `text` with `key` set to `value`, or with `key` removed when `value`
 *  is NULL. Every other byte is preserved: field order, quoting, comments, and
 *  the body. Caller owns the result and frees it with wordsmith_free_string()
 *  from markup-c.h. */
char* wordsmith_frontmatter_set_field(const char* text, const char* key,
                                      const char* value);

#ifdef __cplusplus
}
#endif

#endif /* WORDSMITH_FRONTMATTER_C_H */
