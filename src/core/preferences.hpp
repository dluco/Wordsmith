//
// Settings that belong to the person, not to the manuscript.
//
// `project.wordsmith` describes a project: its title, where the manuscript
// folder is. Preferences describe how this installation of Wordsmith is set up
// to be read and typed in. The split matters because a project directory
// travels (copied to another machine, shared, put in version control), and
// carrying one author's font size along with it would be wrong on arrival.
//
// So preferences live outside every project, in the XDG config directory:
// `$XDG_CONFIG_HOME/wordsmith/settings.json`, or `~/.config/wordsmith/` when
// that variable is unset. One file for the whole application; there is no
// per-window or per-project override, and a second window opened later shows
// the size the last one was left at.
//
// Two rules govern reading it:
//
//   - A file that is missing, unreadable, or malformed yields defaults, not an
//     error. This file is not the author's work. Refusing to start over a
//     corrupt config, or reporting a dialog on every launch, costs more than
//     silently falling back to the size everyone starts at anyway.
//   - Values out of range are clamped rather than rejected, for the same
//     reason: a hand-edited `9000` should give a readable window, not a
//     failure.
//
// Saving re-emits the whole file from the struct below, so a key written by a
// newer build is dropped by an older one. `project.wordsmith` behaves the same
// way. If preferences ever grow to where losing an unknown key matters, the
// fix is to keep the parsed document around and edit it, the way frontmatter
// writes do.
//

#ifndef WORDSMITH_PREFERENCES_HPP
#define WORDSMITH_PREFERENCES_HPP

#include <filesystem>
#include <string>

namespace wordsmith {

/* The editor's text size, as a percentage of the font the system hands us.
 *
 * A percentage rather than a scale factor because it round-trips through JSON
 * exactly and reads plainly in a file people are invited to edit: `125` is a
 * quarter larger, where `1.2500000000000002` is an invitation to file a bug.
 * The bounds are wide enough for a wall display and a laptop on a train, and
 * narrow enough that neither end can make the menu unreachable. */
inline constexpr int TEXT_SCALE_MIN_PERCENT = 50;
inline constexpr int TEXT_SCALE_MAX_PERCENT = 300;
inline constexpr int TEXT_SCALE_DEFAULT_PERCENT = 100;

/* Whether misspelled words are marked in the editor.
 *
 * On unless the author has said otherwise, and every way of not saying anything
 * — no file, no key, a hand-edited value of the wrong type — means on. The
 * session's flags are read the same way and for a related reason: the default
 * has to be the answer that costs nothing when it is wrong, and a red line the
 * author did not ask for is a smaller loss than a manuscript quietly going
 * unchecked because a config file was malformed. */
inline constexpr bool SPELL_CHECK_DEFAULT = true;

struct Preferences {
    int  editor_text_scale_percent = TEXT_SCALE_DEFAULT_PERCENT;
    bool spell_check               = SPELL_CHECK_DEFAULT;
};

/** Where the preferences file lives. The file, and the directory holding it,
 *  need not exist. */
std::filesystem::path preferences_path();

/** Read `file`. Never fails: anything unreadable or unparseable gives
 *  defaults, and out-of-range values are clamped. */
Preferences load_preferences(const std::filesystem::path& file);

/** Write `preferences` to `file`, creating the directory if it is not there.
 *  Atomic, like every other write in Wordsmith. Returns false and fills
 *  `error` on failure. */
bool save_preferences(const Preferences& preferences,
                      const std::filesystem::path& file, std::string& error);

/** `percent` brought inside the bounds above. */
int clamp_text_scale(int percent);

} // namespace wordsmith

#endif /* WORDSMITH_PREFERENCES_HPP */
