//
// Keeping the version the author last saw.
//
// `safe-write.hpp` guarantees a write lands whole. It says nothing about
// whether the bytes were right, and in Wordsmith they might not be: saving a
// document is a round trip through `markup.hpp`, which is explicitly not
// byte-for-byte lossless, and writing metadata is surgical byte-range surgery
// in `yaml.cpp`. Both are places where a bug produces a well-formed, fully
// committed, wrong document. Nothing in an atomic rename can catch that — by
// the time the file is wrong, the only copy of what it used to say is gone.
//
// So before a document is overwritten, what is on disk is copied aside. Five of
// those are kept per document, oldest evicted first.
//
// Snapshots live in `.wordsmith/snapshots/` at the project root, mirroring the
// project's own layout, with each document getting a directory of its versions:
//
//     .wordsmith/snapshots/manuscript/act-one/scene.md/20260731T190549Z-00.md
//
// Central rather than scattered beside each document, so that the disk they
// cost can be seen in one place and swept in one place. Mirroring the layout
// rather than hashing the path, so the author can find a lost draft with a file
// manager and no help from us — a recovery mechanism that requires the app to
// be working is not much of a recovery mechanism. Names sort chronologically.
//
// Moving a document orphans its snapshots, since the key is where the document
// lives. That is a known gap, not an oversight: the alternative is a stable
// document identity, which the project does not have and which the
// "filesystem is the source of truth" design deliberately avoids.
//
// Two things stop the ring from filling with noise, both of which matter
// because a single inspector interaction can write a document several times:
//
//   - contents identical to the newest snapshot are not stored again,
//   - and neither is anything within `DEFAULT_SNAPSHOT_COOLDOWN` of it.
//
// Without the cooldown, typing a synopsis and picking a status would evict
// every version from before this sitting within seconds, which is precisely the
// version most worth keeping. With it, a burst of edits collapses to one entry
// and the five slots span five stretches of work.
//
// The residual gap: five slots at that cadence cover a working session, not a
// week. A markup bug that ate something three drafts ago is out of reach here,
// and wants version control or an export, not a bigger ring.
//

#ifndef WORDSMITH_SNAPSHOTS_HPP
#define WORDSMITH_SNAPSHOTS_HPP

#include <chrono>
#include <filesystem>
#include <vector>

namespace wordsmith {

/* The hidden directory at the project root holding everything Wordsmith keeps
 * about a project that is not the manuscript itself. Dotted, so the binder's
 * scan skips it wherever the manuscript folder is rooted. */
inline constexpr const char* PRIVATE_FOLDER_NAME = ".wordsmith";
inline constexpr const char* SNAPSHOT_FOLDER_NAME = "snapshots";

/** How many versions of a document are kept. A setting eventually; a constant
 *  for now, and the only reason `capture_snapshot` takes it as an argument. */
inline constexpr int DEFAULT_SNAPSHOT_COUNT = 5;

/** How close to the newest snapshot a write has to be to be folded into it. */
inline constexpr std::chrono::seconds DEFAULT_SNAPSHOT_COOLDOWN{10 * 60};

/** The project directory containing `path`, found by walking up looking for
 *  `project.wordsmith`. Empty when `path` is not inside a project — which is
 *  not an error anywhere here, just a document that gets no snapshots. */
std::filesystem::path find_project_root(const std::filesystem::path& path);

/** Where `document`'s snapshots live. The directory need not exist. Empty when
 *  `document` is outside a project. */
std::filesystem::path snapshot_directory(const std::filesystem::path& document);

/** Every kept version of `document`, oldest first. */
std::vector<std::filesystem::path> snapshots(const std::filesystem::path& document);

/**
 * Copy `document` aside before it is overwritten, then evict all but the
 * newest `keep`. Returns true if a version was stored.
 *
 * False is the ordinary case as often as not — a document that does not exist
 * yet, one whose contents already match the newest snapshot, one written again
 * within `cooldown`, or one outside a project. None of those are failures.
 *
 * Neither is a snapshot that could not be written, and that is the important
 * one: this returns false rather than reporting an error because **a failure
 * here must never block the save it precedes**. The author asked to keep their
 * words. Refusing that because a backup of the previous words could not be made
 * would trade a certain loss for a possible one.
 */
bool capture_snapshot(const std::filesystem::path& document,
                      int                          keep = DEFAULT_SNAPSHOT_COUNT,
                      std::chrono::seconds cooldown = DEFAULT_SNAPSHOT_COOLDOWN);

} // namespace wordsmith

#endif /* WORDSMITH_SNAPSHOTS_HPP */
