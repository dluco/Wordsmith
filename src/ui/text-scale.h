#pragma once

/* The size of the text in the editor pane.
 *
 * This is an application preference rather than a property of a pane: it is
 * saved in the user's config directory (see core/preferences.hpp), it is
 * restored at startup before any window exists, and every window shows the
 * same size. So it is applied the way the stylesheet is, through one CSS
 * provider on the display rewritten with a new `font-size` whenever the size
 * changes, rather than by handing each EditorPanel a font of its own. Only the
 * editor pane is affected; the binder and inspector keep the system font,
 * because this is about reading the manuscript, not the chrome around it.
 *
 * The size is stated as a percentage of the system font rather than an
 * absolute point size, so an author who has already set a large font system
 * wide gets larger still when they ask for it. */

/* One step of the Increase/Decrease verbs. Ten percent is small enough that
 * overshooting costs one keystroke to undo. */
#define TEXT_SCALE_STEP_PERCENT 10

/** Read the saved size and apply it. Call once, after the display's other
 *  assets are loaded. */
void text_scale_init(void);

/** The size in force, as a percentage. */
int text_scale_percent(void);

/** The stylesheet rule that puts the editor's text at `percent`, clamped to
 *  the bounds in core/preferences-c.h. Owned by the caller, freed with
 *  g_free().
 *
 *  Split out from applying it so the rule can be checked without a display:
 *  a typo here would leave the size silently unchanged. */
char* text_scale_css(int percent);

/** Show text at `percent`, clamped to the bounds in core/preferences-c.h, and
 *  save it as the preference.
 *
 *  The new size is applied whether or not it could be saved. Returns 0 and
 *  fills `error` (owned by the caller, freed with wordsmith_free_string) when
 *  the save failed, which is worth reporting: the size is right now and wrong
 *  after a restart. */
int text_scale_set(int percent, char** error);
