#ifndef WORDSMITH_SPELLING_C_H
#define WORDSMITH_SPELLING_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spelling: finding the words in a stretch of text, and asking a dictionary
 * about each one. See spelling.hpp for the rules behind both halves.
 *
 * Two separate things cross here rather than one "check this text" call,
 * because the caller has to put a mark on the page between them: it needs to
 * know where each word is, and it decides for itself which words to skip — the
 * one the author is still typing, and anything inside a code block. */

/* ── the words in a stretch of text ─────────────────────────────────────── */

/* Where a word is and what it says. `offset` and `length` count *characters*,
 * the unit GtkTextIter counts in, so they can be turned into iterators without
 * a conversion that would break at the first accented character. `text` is
 * owned by the array it came in. */
typedef struct WordsmithSpellWord {
    int   offset;
    int   length;
    char* text;
} WordsmithSpellWord;

/** Every word in `text` worth checking, in order. Fills `count`. Returns NULL
 *  when there are none, which is not an error. Free with
 *  wordsmith_spell_words_free(). */
WordsmithSpellWord* wordsmith_spell_words(const char* text, size_t* count);

void wordsmith_spell_words_free(WordsmithSpellWord* words, size_t count);

/* ── the dictionary ─────────────────────────────────────────────────────── */

typedef struct WordsmithSpellChecker WordsmithSpellChecker;

/** Open the system's dictionary for the language the environment asks for.
 *  Never fails: with nothing installed the checker simply knows every word, so
 *  the editor marks nothing. */
WordsmithSpellChecker* wordsmith_spell_checker_new(void);

void wordsmith_spell_checker_free(WordsmithSpellChecker* checker);

/** Whether a dictionary was actually found. Worth asking only to say so in a
 *  test or a log; nothing about the editor needs to change either way. */
int wordsmith_spell_checker_available(const WordsmithSpellChecker* checker);

/** The language that was opened, or "" when none was. Borrowed. */
const char* wordsmith_spell_checker_language(const WordsmithSpellChecker* checker);

/** Whether `word` is spelled the way the dictionary spells it. Nonzero — no
 *  mark — whenever there is no dictionary to ask. */
int wordsmith_spell_checker_knows(const WordsmithSpellChecker* checker,
                                  const char* word);

/** What `word` might have been meant to be, best first. Fills `count`, and
 *  returns NULL when there is nothing to offer. Free with
 *  wordsmith_spell_corrections_free(). */
char** wordsmith_spell_checker_corrections(const WordsmithSpellChecker* checker,
                                           const char* word, size_t* count);

void wordsmith_spell_corrections_free(char** corrections, size_t count);

/** Accept `word` for this sitting, writing nothing to disk. The door the
 *  manuscript's own names and invented terms will come through. */
void wordsmith_spell_checker_accept(WordsmithSpellChecker* checker,
                                    const char* word);

/** Accept `word` for good, in this user's personal word list. */
void wordsmith_spell_checker_remember(WordsmithSpellChecker* checker,
                                      const char* word);

#ifdef __cplusplus
}
#endif

#endif /* WORDSMITH_SPELLING_C_H */
