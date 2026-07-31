//
// The project model.
//
// A Wordsworth project is an ordinary directory containing a `project.wordsworth`
// JSON file and a manuscript folder of Markdown files. The filesystem is the
// source of truth for the binder: the JSON names the manuscript folder and
// carries project-wide settings, but it does not enumerate documents.
//
// That choice is deliberate but not permanent. Scrivener and Ulysses index
// everything in a central file, which is what lets them do manual reordering
// and per-document metadata; editors like Obsidian and iA Writer let the
// directory speak for itself. Wordsworth starts as the second kind. The seam
// for changing that is `load_binder()`: swap its implementation and everything
// above keeps working.
//

#ifndef WORDSWORTH_PROJECT_HPP
#define WORDSWORTH_PROJECT_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wordsworth {

/* The file naming a directory as a project, and the default manuscript folder
 * created inside a new one. */
inline constexpr const char* PROJECT_FILE_NAME = "project.wordsworth";
inline constexpr const char* DEFAULT_MANUSCRIPT_FOLDER = "manuscript";
inline constexpr const char* DOCUMENT_EXTENSION = ".md";

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
 * Folders sort before documents, then both by name, case-insensitively. Only
 * `.md` files are included; anything else on disk is ignored rather than shown,
 * so a stray `.DS_Store` or an image folder does not clutter the binder.
 *
 * This function is the seam described in the file comment. Everything that
 * builds a binder goes through here.
 */
BinderEntry load_binder(const std::filesystem::path& manuscript_root);

/** An open project. Construct with `open` or `create`, both of which return
 *  null and fill `error` on failure. */
class Project {
public:
    /** Open the project rooted at `root`, the directory holding
     *  project.wordsworth. */
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

    /** Write project.wordsworth back out. */
    bool save_settings(std::string& error) const;

    /** Create a subfolder of `parent`. `parent` must be the manuscript folder
     *  or a folder inside it. Does not rescan. */
    bool create_folder(const std::filesystem::path& parent, std::string_view name,
                       std::string& error) const;

    /** Create an empty `.md` document in `parent`, returning its path through
     *  `created_path`. Does not rescan. */
    bool create_document(const std::filesystem::path& parent, std::string_view name,
                         std::filesystem::path& created_path,
                         std::string& error) const;

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

/** Write a document's Markdown, replacing whatever was there. */
bool write_document(const std::filesystem::path& path, std::string_view markdown,
                    std::string& error);

/** Turn a user-typed title into a filesystem-safe base name. Collapses runs of
 *  unsafe characters to single hyphens and trims them from the ends. Returns
 *  "untitled" if nothing usable survives. */
std::string sanitize_name(std::string_view title);

} // namespace wordsworth

#endif /* WORDSWORTH_PROJECT_HPP */
