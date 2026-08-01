//
// The project model.
//
// A Wordsmith project is an ordinary directory containing a `project.wordsmith`
// JSON file and a manuscript folder of Markdown files. The filesystem is the
// source of truth for the binder: the JSON names the manuscript folder and
// carries project-wide settings, but it does not enumerate documents.
//
// That choice is deliberate but not permanent. Scrivener and Ulysses index
// everything in a central file, which is what lets them do manual reordering
// and per-document metadata; editors like Obsidian and iA Writer let the
// directory speak for itself. Wordsmith starts as the second kind. The seam
// for changing that is `load_binder()`: swap its implementation and everything
// above keeps working.
//

#ifndef WORDSMITH_PROJECT_HPP
#define WORDSMITH_PROJECT_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wordsmith {

/* The file naming a directory as a project, and the default manuscript folder
 * created inside a new one. */
inline constexpr const char* PROJECT_FILE_NAME = "project.wordsmith";
inline constexpr const char* DEFAULT_MANUSCRIPT_FOLDER = "manuscript";
inline constexpr const char* DOCUMENT_EXTENSION = ".md";

/* A folder's sidecar, sitting inside the folder it describes.
 *
 * A document carries its metadata in its own frontmatter, but a folder has no
 * file to put it in, so it gets one. The sidecar holds the folder's own fields
 * — synopsis and the rest of the same vocabulary — and a `children:` list
 * giving the order of its immediate children.
 *
 * Ordering lives here, distributed a folder at a time, rather than in one index
 * at the top of the project. Position is a property of the relationship between
 * an item and its siblings, not of the item, and a folder *is* the sibling set:
 * keeping the order inside it means moving a folder carries its ordering along
 * with it, with no paths anywhere to rewrite and nothing to go stale when the
 * move happens outside Wordsmith. It also confines a merge conflict to the one
 * folder two people both reordered.
 *
 * The list is a hint, never an authority — see `load_binder`. */
inline constexpr const char* FOLDER_METADATA_FILE_NAME = "metadata.yaml";

/** One node in the binder tree. Folders carry children; documents do not. */
struct BinderEntry {
    std::string              name;       // display name: filename stem, or folder name
    std::filesystem::path    path;       // absolute
    std::string              path_string; // `path` as a string, for the C bridge
    bool                     is_folder = false;
    std::vector<BinderEntry> children;
};

/**
 * Scan `manuscript_root` into a binder tree. The returned entry is a synthetic
 * root standing for the manuscript folder itself.
 *
 * Only `.md` files are included; anything else on disk is ignored rather than
 * shown, so a stray `.DS_Store`, an image folder, or a folder's own
 * metadata.yaml does not clutter the binder.
 *
 * Order comes from each folder's `children:` list, and that list can only ever
 * reorder what the scan found — it cannot assert that something exists. In
 * full:
 *
 *   1. the directory scan alone decides what is in the binder,
 *   2. entries named by the list come first, in the list's order,
 *   3. whatever the list does not mention follows, folders before documents
 *      and then case-insensitively by name,
 *   4. names in the list with nothing behind them are ignored.
 *
 * So deleting a chapter outside Wordsmith makes it vanish, adding one makes it
 * appear at the end, and a hand-edited list that has drifted gives a
 * mis-ordered binder rather than a broken one.
 *
 * This function is the seam described in the file comment. Everything that
 * builds a binder goes through here.
 */
BinderEntry load_binder(const std::filesystem::path& manuscript_root);

/** Where `folder` keeps its sidecar. The file need not exist. */
std::filesystem::path folder_metadata_path(const std::filesystem::path& folder);

/** The child order recorded for `folder`, or empty when it records none.
 *  Entries are filenames as written: `chapter-one.md`, or a bare name for a
 *  subfolder. */
std::vector<std::string> read_child_order(const std::filesystem::path& folder);

/** Record `order` as `folder`'s child order, leaving every other field in the
 *  sidecar untouched. Creates the sidecar if there is none. */
bool write_child_order(const std::filesystem::path& folder,
                       const std::vector<std::string>& order, std::string& error);

/** An open project. Construct with `open` or `create`, both of which return
 *  null and fill `error` on failure. */
class Project {
public:
    /** Open the project rooted at `root`, the directory holding
     *  project.wordsmith. */
    static std::unique_ptr<Project> open(const std::filesystem::path& root,
                                         std::string& error);

    /** Create a new project directory at `root`, with a project file and an
     *  empty manuscript folder. Fails if `root` already holds a project. */
    static std::unique_ptr<Project> create(const std::filesystem::path& root,
                                           std::string_view title,
                                           std::string& error);

    const std::filesystem::path& root() const { return root_; }
    const std::string&           title() const { return title_; }
    std::filesystem::path        manuscript_path() const;

    /** The binder as of the last scan. */
    const BinderEntry& binder() const { return binder_; }

    /** Rescan the manuscript folder. Call after anything changes on disk. */
    void reload_binder();

    /** Write project.wordsmith back out. */
    bool save_settings(std::string& error) const;

    /** Create a subfolder of `parent`, returning its path through
     *  `created_path`. `parent` must be the manuscript folder or a folder
     *  inside it. Does not rescan. */
    bool create_folder(const std::filesystem::path& parent, std::string_view name,
                       std::filesystem::path& created_path,
                       std::string& error) const;

    /** Create an empty `.md` document in `parent`, returning its path through
     *  `created_path`. Does not rescan. */
    bool create_document(const std::filesystem::path& parent, std::string_view name,
                         std::filesystem::path& created_path,
                         std::string& error) const;

    /** Record the order of `folder`'s children. `folder` must be the manuscript
     *  folder or one inside it. Does not rescan. */
    bool set_child_order(const std::filesystem::path& folder,
                         const std::vector<std::string>& order,
                         std::string& error) const;

    /** Create a folder called `name` beside `item` and move `item` into it,
     *  reporting both paths back.
     *
     *  Where the parent records an order, the new folder takes the place the
     *  item held, rather than landing at the end: a group stands where the
     *  thing it gathered up used to stand. Does not rescan. */
    bool group_into_new_folder(const std::filesystem::path& item,
                               std::string_view name,
                               std::filesystem::path& folder_path,
                               std::filesystem::path& moved_path,
                               std::string& error) const;

    /** Move `source` into `anchor`'s folder and place it immediately before or
     *  after `anchor`, reporting where it landed through `moved_path`.
     *
     *  This is the verb behind a drag: the gesture says "put this next to
     *  that", and where that falls in the list is the core's problem, not the
     *  caller's. Unlike the plain `move_entry`, it writes the destination's
     *  order down and creates the sidecar if there is none — asking for a
     *  position is exactly the moment a folder becomes arranged. Dropping
     *  something beside itself succeeds without doing anything. Does not
     *  rescan. */
    bool move_entry_beside(const std::filesystem::path& source,
                           const std::filesystem::path& anchor, bool after,
                           std::filesystem::path& moved_path,
                           std::string& error) const;

    /** Move `source` into `destination_parent`, keeping its name, and report
     *  where it landed through `moved_path`.
     *
     *  Both ends must lie inside the manuscript folder, and a folder cannot be
     *  moved into itself or any of its descendants — `rename` would happily
     *  detach the subtree, so the check has to live here. Moving something to
     *  where it already is succeeds without touching the disk. Does not
     *  rescan. */
    bool move_entry(const std::filesystem::path& source,
                    const std::filesystem::path& destination_parent,
                    std::filesystem::path& moved_path, std::string& error) const;

    /** Rename `item` to `new_name`, reporting where it landed through
     *  `renamed_path`.
     *
     *  `new_name` is a user-typed title and goes through `sanitize_name()` the
     *  way a created item's does, so what the author types is not always what
     *  the file is called. That is the honest answer here rather than a
     *  concession: the binder shows filenames, and a name that cannot be a
     *  filename is not a name this can give. The `title` field in a document's
     *  frontmatter is a separate thing and is deliberately left alone — one
     *  gesture changes one name.
     *
     *  Where the parent records an order, the item **keeps its place** in it
     *  rather than falling to the end. A rename is not a move, and an author who
     *  fixes a typo in chapter three should not find it after chapter twelve.
     *
     *  Renaming to the name it already has succeeds without touching the disk.
     *  The manuscript folder itself cannot be renamed: it is named by
     *  `project.wordsmith`, so moving it would need that file rewritten in the
     *  same breath. Does not rescan.
     *
     *  Snapshots are keyed by where a document lives, so renaming one orphans
     *  its previous versions in `.wordsmith/snapshots/` — the same known gap
     *  moving one has, and for the same reason. See `snapshots.hpp`. */
    bool rename_entry(const std::filesystem::path& item, std::string_view new_name,
                      std::filesystem::path& renamed_path, std::string& error) const;

    /** True if `path` lies inside the manuscript folder. Guards the mutating
     *  calls above against a caller passing an arbitrary path. */
    bool contains(const std::filesystem::path& path) const;

private:
    Project() = default;

    std::filesystem::path root_;
    std::string           title_;
    std::string           manuscript_folder_ = DEFAULT_MANUSCRIPT_FOLDER;
    BinderEntry           binder_;
};

/** Read a document's Markdown. Returns false and fills `error` on failure. */
bool read_document(const std::filesystem::path& path, std::string& out,
                   std::string& error);

/** Write a document's Markdown, replacing whatever was there.
 *
 *  Atomic, and snapshots the previous contents first — see `safe-write.hpp` and
 *  `snapshots.hpp` for what each of those does and does not cover. Everything
 *  that writes a document or a sidecar goes through here, which is what makes
 *  those two guarantees hold everywhere rather than at most call sites. */
bool write_document(const std::filesystem::path& path, std::string_view markdown,
                    std::string& error);

/** Turn a user-typed title into a filesystem-safe base name. Collapses runs of
 *  unsafe characters to single hyphens and trims them from the ends. Returns
 *  "untitled" if nothing usable survives. */
std::string sanitize_name(std::string_view title);

} // namespace wordsmith

#endif /* WORDSMITH_PROJECT_HPP */
