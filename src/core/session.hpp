//
// What the author was looking at, remembered between sittings.
//
// Reopening a project should put it back the way it was left: the same folders
// twisted open in the binder, the same document in the editor. That is neither
// the manuscript nor a preference, and it does not belong with either.
//
// Not in `project.wordsmith`, because that file describes the work. Expanded
// rows would churn it on every click, fill a diff with noise, and give two
// people sharing a project a quiet argument about whose binder is open.
//
// Not in the settings.json `preferences.hpp` owns either. That file is config:
// the deliberate answers one author gives about how Wordsmith should look, in a
// file they are invited to open and edit. This is state, restored without ever
// having been asked for, and there is one entry per project rather than one for
// the whole application.
//
// So it lives in the XDG *state* directory, `$XDG_STATE_HOME/wordsmith/
// session.json`, or `~/.local/state/wordsmith/` when that variable is unset.
// Outside every project, for the reason preferences.hpp gives: a project
// directory travels, and which rows one author had unfolded should not arrive
// with it.
//
// One file holds every project, keyed by the project root, most recently used
// first and capped at SESSION_PROJECT_LIMIT entries. One file rather than one
// per project because there is then nothing to collect when a project is
// deleted, and the cap keeps the file from growing without bound. The order is
// also most of a recent-projects list, if one is ever wanted.
//
// Within an entry, paths are written relative to the project root. Only the
// root has to be spelled out in full, since it is the key; keeping the interior
// short makes the file legible and leaves the entry intact if the key ever
// learns to follow a project that moved.
//
// Reading obeys the two rules preferences does, for the same reason: nothing
// here is the author's work, and refusing to open a project over it would cost
// more than starting from a collapsed binder.
//
//   - A file that is missing, unreadable or malformed reads as no saved state.
//   - A recorded path with nothing behind it on disk is dropped rather than
//     restored. This is the `children:` contract from project.hpp again: what
//     is written down can only reorder or reopen what the filesystem already
//     has, never assert that it exists. A chapter deleted outside Wordsmith
//     opens nothing, rather than raising an error on launch.
//
// Saving re-emits the whole file through `write_file_atomically()`, so a key a
// newer build wrote is dropped by an older one, the same trade preferences and
// `project.wordsmith` both make.
//

#ifndef WORDSMITH_SESSION_HPP
#define WORDSMITH_SESSION_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace wordsmith {

/* How many projects are remembered. Past this the least recently opened falls
 * off the end. Large enough that an author cycling between the books they have
 * in flight never loses a binder, small enough that the file stays a file
 * somebody could read. */
inline constexpr std::size_t SESSION_PROJECT_LIMIT = 50;

/** What was on screen for one project. `open_document` and the entries of
 *  `expanded` are relative to `root`, spelled with '/' separators;
 *  `open_document` is empty when no document was open. */
struct ProjectSession {
    std::string              root;
    std::string              open_document;
    std::vector<std::string> expanded;
};

/** Where the session file lives. The file, and the directory holding it, need
 *  not exist. */
std::filesystem::path session_path();

/** The spelling of a project root used to key an entry. Absolute and
 *  normalised, so the same project opened by two different paths is one entry.
 *  Falls back to `root` unchanged if the filesystem cannot answer. */
std::filesystem::path session_key(const std::filesystem::path& root);

/** `item` written relative to `root`, or empty when it does not lie inside
 *  `root`. */
std::string session_relative(const std::filesystem::path& root,
                             const std::filesystem::path& item);

/** The reverse of `session_relative`. Resolved against `root` as spelled, not
 *  against its canonical form, so the result matches the paths the rest of the
 *  project hands around. Empty in gives empty out. */
std::filesystem::path session_absolute(const std::filesystem::path& root,
                                       const std::string& relative);

/** Every remembered project, most recently opened first. Never fails: anything
 *  unreadable or unparseable reads as an empty list. Entries come back as
 *  written, including any that no longer exist on disk. */
std::vector<ProjectSession> load_session(const std::filesystem::path& file);

/** What was saved for the project at `root`, with anything no longer on disk
 *  dropped. A project that has never been opened, like one whose entry has gone
 *  entirely stale, comes back with `root` filled in and nothing else. */
ProjectSession session_for(const std::filesystem::path& file,
                           const std::filesystem::path& root);

/** Record `session`, moving its project to the front and dropping the oldest
 *  entries past SESSION_PROJECT_LIMIT. Creates the directory if it is not
 *  there. Atomic, like every other write in Wordsmith. Returns false and fills
 *  `error` on failure. */
bool save_project_session(const ProjectSession& session,
                          const std::filesystem::path& file, std::string& error);

} // namespace wordsmith

#endif /* WORDSMITH_SESSION_HPP */
