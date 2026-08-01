#ifndef WORDSMITH_SESSION_C_H
#define WORDSMITH_SESSION_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-project view state: which binder folders were open, which document was
 * being edited, and whether the inspector was on screen. See session.hpp for
 * where the file lives, and why a project with nothing saved is never an error.
 *
 * Paths cross this bridge as absolute paths, spelled against the `root` handed
 * in, because that is how the rest of the UI spells them. The relative form
 * that actually goes in the file is the core's business. */

typedef struct WordsmithSession WordsmithSession;

/** What was last on screen for the project rooted at `root`. Never NULL: a
 *  project nothing was saved for comes back empty. Anything recorded that is no
 *  longer on disk has already been dropped. Free with wordsmith_session_free(). */
WordsmithSession* wordsmith_session_load(const char* root);

void wordsmith_session_free(WordsmithSession* session);

/** The document that was open, or NULL if there was none. Borrowed, valid until
 *  the session is freed. */
const char* wordsmith_session_open_document(const WordsmithSession* session);

/* The folders that were expanded. Borrowed on the same terms. */
size_t      wordsmith_session_expanded_count(const WordsmithSession* session);
const char* wordsmith_session_expanded(const WordsmithSession* session, size_t index);

/** Whether the inspector was on screen. A project nothing was saved for, like
 *  one saved by a build without the field, answers 1: the pane starts on
 *  screen, and a missing answer must not put it away. */
int wordsmith_session_inspector_visible(const WordsmithSession* session);

/** Record what is on screen for the project rooted at `root`. `open_document`
 *  may be NULL, and `expanded` may be NULL when `count` is 0. Paths outside
 *  `root` are ignored rather than refused.
 *
 *  Returns 0 and fills `error` (owned by the caller, freed with
 *  wordsmith_free_string) when the file cannot be written. */
int wordsmith_session_save(const char* root, const char* open_document,
                           const char* const* expanded, size_t count,
                           int inspector_visible, char** error);

#ifdef __cplusplus
}
#endif

#endif /* WORDSMITH_SESSION_C_H */
