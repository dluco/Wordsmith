#ifndef WORDSMITH_PREFERENCES_C_H
#define WORDSMITH_PREFERENCES_C_H

#ifdef __cplusplus
extern "C" {
#endif

/* Application preferences, read from and written to the user's config
 * directory. See preferences.hpp for where the file lives and why a bad one is
 * never an error.
 *
 * There is no handle here: preferences are one small file describing the whole
 * application, and the UI reads them once at startup and writes them when
 * something changes. Passing an object around windows would suggest each
 * window could hold a different answer. */

/* Bounds on the editor's text size, as a percentage of the system font. */
#define WORDSMITH_TEXT_SCALE_MIN_PERCENT     50
#define WORDSMITH_TEXT_SCALE_MAX_PERCENT     300
#define WORDSMITH_TEXT_SCALE_DEFAULT_PERCENT 100

/** The saved editor text size, or the default when nothing is saved. Always
 *  within the bounds above. */
int wordsmith_preferences_text_scale(void);

/** Record `percent` as the editor text size, clamped to the bounds above.
 *  Returns 0 and fills `error` (owned by the caller, freed with
 *  wordsmith_free_string) when the file cannot be written. */
int wordsmith_preferences_set_text_scale(int percent, char** error);

#ifdef __cplusplus
}
#endif

#endif /* WORDSMITH_PREFERENCES_C_H */
