#include "project.hpp"

#include "safe-write.hpp"
#include "snapshots.hpp"
#include "yaml.hpp"

#include <argo/argo.hpp>
#include <argo/exceptions.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace wordsmith {

namespace {

/* Bumped when the on-disk shape changes in a way older builds cannot read. */
constexpr std::int64_t PROJECT_FORMAT_VERSION = 1;

std::string lowercase(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

/* Folders first, then case-insensitive name order. Ties break on the raw name
 * so the sort stays deterministic across filesystems. */
bool binder_order(const BinderEntry& left, const BinderEntry& right)
{
    if (left.is_folder != right.is_folder) {
        return left.is_folder;
    }
    const std::string left_key = lowercase(left.name);
    const std::string right_key = lowercase(right.name);
    if (left_key != right_key) {
        return left_key < right_key;
    }
    return left.name < right.name;
}

bool is_document(const fs::path& path)
{
    return lowercase(path.extension().string()) == DOCUMENT_EXTENSION;
}

/* Whether `candidate` is `ancestor` or sits beneath it. Compares canonical
 * components rather than strings, so `a/../b` and a trailing slash do not
 * change the answer. */
bool path_is_within(const fs::path& ancestor, const fs::path& candidate)
{
    std::error_code code;
    const fs::path top = fs::weakly_canonical(ancestor, code);
    const fs::path below = fs::weakly_canonical(candidate, code);
    if (code) {
        return false;
    }

    auto top_part = top.begin();
    auto below_part = below.begin();
    for (; top_part != top.end(); ++top_part, ++below_part) {
        if (below_part == below.end() || *below_part != *top_part) {
            return false;
        }
    }
    return true;
}

/* Whether two paths name the same place, spelling aside. */
bool paths_equal(const fs::path& left, const fs::path& right)
{
    std::error_code code;
    const fs::path a = fs::weakly_canonical(left, code);
    const fs::path b = fs::weakly_canonical(right, code);
    return !code && a == b;
}

/* `folder/<stem><extension>` while that is free, then `<stem>-2<extension>`,
 * `<stem>-3<extension>` and so on. The counter goes before the extension so a
 * document is still a `.md` file a file manager will open on a double click.
 *
 * Bounded rather than looping until it finds a gap: an `fs::exists` that kept
 * answering yes would otherwise spin forever. Past the cap the last candidate
 * comes back regardless, and whatever the caller does with it fails with a real
 * message about a real path, which is a better answer than a hang. */
fs::path unused_path(const fs::path& folder, const std::string& stem,
                     const std::string& extension)
{
    constexpr int MAX_ATTEMPTS = 1000;

    fs::path candidate = folder / (stem + extension);
    std::error_code code;
    if (!fs::exists(candidate, code)) {
        return candidate;
    }

    for (int counter = 2; counter < MAX_ATTEMPTS; counter++) {
        candidate = folder / (stem + "-" + std::to_string(counter) + extension);
        if (!fs::exists(candidate, code)) {
            break;
        }
    }
    return candidate;
}

} // namespace

/* ── folder metadata ────────────────────────────────────────────────────── */

fs::path folder_metadata_path(const fs::path& folder)
{
    return folder / FOLDER_METADATA_FILE_NAME;
}

std::vector<std::string> read_child_order(const fs::path& folder)
{
    std::string text;
    std::string error;
    if (!read_document(folder_metadata_path(folder), text, error)) {
        return {};
    }

    const yaml::Node root = yaml::parse(text);
    const yaml::Node* children = root.find("children");
    if (children == nullptr || !children->is_sequence()) {
        return {};
    }

    std::vector<std::string> order;
    order.reserve(children->seq.size());
    for (const yaml::Node& child : children->seq) {
        if (!child.scalar.empty()) {
            order.push_back(child.scalar);
        }
    }
    return order;
}

bool write_child_order(const fs::path& folder, const std::vector<std::string>& order,
                       std::string& error)
{
    const fs::path path = folder_metadata_path(folder);

    /* Read-modify-write rather than emit: the sidecar holds the folder's
     * synopsis too, and rewriting only the `children:` range leaves that, its
     * comments, and its field order exactly as the author left them. */
    std::string text;
    std::string ignored;
    if (!read_document(path, text, ignored)) {
        text.clear();
    }

    return write_document(path, yaml::set_sequence(text, "children", order), error);
}

/* ── binder ─────────────────────────────────────────────────────────────── */

namespace {

/* Reorder `entries` to match `order`, which names them by filename. Anything
 * `order` does not mention keeps its place at the end, and anything it names
 * that is not here is dropped: the list reorders reality, it does not assert
 * it. A bare name also matches a document's stem, so a hand-written
 * `chapter-one` finds `chapter-one.md`. */
void apply_child_order(std::vector<BinderEntry>& entries,
                       const std::vector<std::string>& order)
{
    if (order.empty() || entries.empty()) {
        return;
    }

    std::vector<BinderEntry> sorted;
    sorted.reserve(entries.size());
    std::vector<bool> taken(entries.size(), false);

    for (const std::string& wanted : order) {
        std::size_t match = entries.size();
        for (std::size_t index = 0; index < entries.size(); index++) {
            if (taken[index]) {
                continue;
            }
            const fs::path& path = entries[index].path;
            if (path.filename().string() == wanted) {
                match = index;
                break;
            }
            if (match == entries.size() && path.stem().string() == wanted) {
                match = index;   // keep looking for an exact filename first
            }
        }
        if (match < entries.size()) {
            sorted.push_back(std::move(entries[match]));
            taken[match] = true;
        }
    }

    for (std::size_t index = 0; index < entries.size(); index++) {
        if (!taken[index]) {
            sorted.push_back(std::move(entries[index]));
        }
    }
    entries = std::move(sorted);
}

/* The immediate children of `folder`, filtered and ordered as the binder shows
 * them, with folders left unexpanded. Both the recursive scan and anything that
 * needs to know a folder's running order come through here, so there is one
 * answer to "what is in this folder, and in what order". */
std::vector<BinderEntry> ordered_children(const fs::path& folder)
{
    std::vector<BinderEntry> entries;

    std::error_code code;
    if (!fs::is_directory(folder, code)) {
        return entries;
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(folder, code)) {
        const fs::path& path = entry.path();
        // Skip dotfiles: editor cruft and, later, our own sidecars.
        if (!path.filename().empty() && path.filename().string()[0] == '.') {
            continue;
        }

        BinderEntry child;
        if (entry.is_directory(code)) {
            child.is_folder = true;
            child.name      = path.filename().string();
        } else if (entry.is_regular_file(code) && is_document(path)) {
            child.name = path.stem().string();
        } else {
            continue;
        }
        child.path        = path;
        child.path_string = path.string();
        entries.push_back(std::move(child));
    }

    /* Alphabetical first, so that whatever the sidecar does not mention still
     * lands somewhere predictable. */
    std::sort(entries.begin(), entries.end(), binder_order);
    apply_child_order(entries, read_child_order(folder));
    return entries;
}

/* The same list, named the way the sidecar names things. */
std::vector<std::string> ordered_child_names(const fs::path& folder)
{
    const std::vector<BinderEntry> entries = ordered_children(folder);
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const BinderEntry& entry : entries) {
        names.push_back(entry.path.filename().string());
    }
    return names;
}

} // namespace

BinderEntry load_binder(const fs::path& manuscript_root)
{
    BinderEntry root;
    root.name        = manuscript_root.filename().string();
    root.path        = manuscript_root;
    root.path_string = manuscript_root.string();
    root.is_folder   = true;
    root.children    = ordered_children(manuscript_root);

    for (BinderEntry& child : root.children) {
        if (child.is_folder) {
            child = load_binder(child.path);
        }
    }
    return root;
}

/* ── project ────────────────────────────────────────────────────────────── */

std::unique_ptr<Project> Project::open(const fs::path& root, std::string& error)
{
    const fs::path project_file = root / PROJECT_FILE_NAME;

    std::error_code code;
    if (!fs::is_regular_file(project_file, code)) {
        error = "no " + std::string(PROJECT_FILE_NAME) + " in " + root.string();
        return nullptr;
    }

    std::string text;
    if (!read_document(project_file, text, error)) {
        return nullptr;
    }

    argo::json document;
    try {
        document = argo::json::parse(text);
    } catch (const argo::json_error& failure) {
        error = std::string("malformed ") + PROJECT_FILE_NAME + ": " + failure.what();
        return nullptr;
    }

    if (!document.is_object()) {
        error = std::string(PROJECT_FILE_NAME) + " must contain a JSON object";
        return nullptr;
    }

    auto project = std::unique_ptr<Project>(new Project());
    project->root_ = fs::absolute(root, code);

    const std::map<std::string, argo::json>& fields = document.get_object();

    auto version = fields.find("version");
    if (version != fields.end() && version->second.is_number()
        && version->second.get_integer() > PROJECT_FORMAT_VERSION) {
        error = "project was written by a newer version of Wordsmith";
        return nullptr;
    }

    auto title = fields.find("title");
    if (title != fields.end() && title->second.is_string()) {
        project->title_ = title->second.get_string();
    }
    if (project->title_.empty()) {
        project->title_ = project->root_.filename().string();
    }

    auto manuscript = fields.find("manuscript");
    if (manuscript != fields.end() && manuscript->second.is_string()
        && !manuscript->second.get_string().empty()) {
        project->manuscript_folder_ = manuscript->second.get_string();
    }

    project->reload_binder();
    return project;
}

std::unique_ptr<Project> Project::create(const fs::path& root, std::string_view title,
                                         std::string& error)
{
    std::error_code code;
    if (fs::exists(root / PROJECT_FILE_NAME, code)) {
        error = root.string() + " already holds a project";
        return nullptr;
    }

    fs::create_directories(root, code);
    if (code) {
        error = "cannot create " + root.string() + ": " + code.message();
        return nullptr;
    }

    auto project = std::unique_ptr<Project>(new Project());
    project->root_ = fs::absolute(root, code);
    project->title_ = title.empty() ? project->root_.filename().string()
                                    : std::string(title);

    fs::create_directory(project->manuscript_path(), code);
    if (code) {
        error = "cannot create manuscript folder: " + code.message();
        return nullptr;
    }

    if (!project->save_settings(error)) {
        return nullptr;
    }

    project->reload_binder();
    return project;
}

fs::path Project::manuscript_path() const
{
    return root_ / manuscript_folder_;
}

void Project::reload_binder()
{
    binder_ = load_binder(manuscript_path());
}

bool Project::save_settings(std::string& error) const
{
    /* argo serializes compactly, so this lands on one line. The file is small
     * enough to stay readable; pretty-printing belongs in argo if it is ever
     * wanted. */
    argo::json document = argo::json::object({
        {"version", argo::json::integer_value(PROJECT_FORMAT_VERSION)},
        {"title", argo::json::string(title_)},
        {"manuscript", argo::json::string(manuscript_folder_)},
    });

    return write_file_atomically(root_ / PROJECT_FILE_NAME,
                                 document.serialize() + "\n", error);
}

bool Project::contains(const fs::path& path) const
{
    return path_is_within(manuscript_path(), path);
}

bool Project::create_folder(const fs::path& parent, std::string_view name,
                            fs::path& created_path, std::string& error) const
{
    if (!contains(parent)) {
        error = "target folder is outside the manuscript";
        return false;
    }

    const fs::path target = parent / sanitize_name(name);
    std::error_code code;
    if (fs::exists(target, code)) {
        error = target.filename().string() + " already exists";
        return false;
    }
    if (!fs::create_directory(target, code)) {
        error = "cannot create folder: " + code.message();
        return false;
    }
    created_path = target;
    return true;
}

namespace {

/* Both of these leave a folder that records no order alone. A sidecar exists
 * because someone arranged that folder; creating one just to write down the
 * position an unlisted item would land in anyway would put a metadata.yaml in
 * every folder anyone ever moved anything into. */
bool records_order(const fs::path& folder)
{
    std::error_code code;
    return fs::is_regular_file(folder_metadata_path(folder), code);
}

void forget_child(const fs::path& folder, const std::string& name)
{
    if (!records_order(folder)) {
        return;
    }
    std::vector<std::string> order = read_child_order(folder);
    const std::size_t before = order.size();
    order.erase(std::remove(order.begin(), order.end(), name), order.end());
    if (order.size() != before) {
        std::string ignored;
        write_child_order(folder, order, ignored);
    }
}

void remember_child(const fs::path& folder, const std::string& name)
{
    if (!records_order(folder)) {
        return;
    }
    std::vector<std::string> order = read_child_order(folder);
    if (std::find(order.begin(), order.end(), name) != order.end()) {
        return;
    }
    order.push_back(name);
    std::string ignored;
    write_child_order(folder, order, ignored);
}

/* In place, rather than forgetting the old name and remembering the new one:
 * that pair would take the item out of the middle of the list and put it back at
 * the end, which is a move. Renaming something leaves it where it stands. */
void rename_child(const fs::path& folder, const std::string& old_name,
                  const std::string& new_name)
{
    if (!records_order(folder)) {
        return;
    }
    std::vector<std::string> order = read_child_order(folder);
    const auto at = std::find(order.begin(), order.end(), old_name);
    if (at == order.end()) {
        return;
    }
    *at = new_name;

    std::string ignored;
    write_child_order(folder, order, ignored);
}

} // namespace

bool Project::set_child_order(const fs::path& folder,
                              const std::vector<std::string>& order,
                              std::string& error) const
{
    if (!contains(folder)) {
        error = "folder is outside the manuscript";
        return false;
    }
    std::error_code code;
    if (!fs::is_directory(folder, code)) {
        error = "not a folder";
        return false;
    }
    return write_child_order(folder, order, error);
}

bool Project::group_into_new_folder(const fs::path& item, std::string_view name,
                                    fs::path& folder_path, fs::path& moved_path,
                                    std::string& error) const
{
    if (!contains(item)) {
        error = "the item being grouped is outside the manuscript";
        return false;
    }

    const fs::path    parent    = item.parent_path();
    const std::string item_name = item.filename().string();

    /* Note where the item stood before the move takes it out of the list. */
    const std::vector<std::string> before = read_child_order(parent);
    const auto at = std::find(before.begin(), before.end(), item_name);
    const bool  ordered = at != before.end();
    const std::size_t position =
        ordered ? static_cast<std::size_t>(std::distance(before.begin(), at)) : 0;

    if (!create_folder(parent, name, folder_path, error)) {
        return false;
    }
    if (!move_entry(item, folder_path, moved_path, error)) {
        /* The folder stays behind. There is no delete verb to take it back,
         * and an empty folder is a smaller surprise than a half-done move. */
        return false;
    }

    if (ordered) {
        std::vector<std::string> after = read_child_order(parent);
        const std::string folder_name = folder_path.filename().string();
        after.erase(std::remove(after.begin(), after.end(), folder_name), after.end());
        after.insert(after.begin()
                         + static_cast<std::ptrdiff_t>(
                               position < after.size() ? position : after.size()),
                     folder_name);

        std::string ignored;
        write_child_order(parent, after, ignored);
    }
    return true;
}

bool Project::move_entry(const fs::path& source, const fs::path& destination_parent,
                         fs::path& moved_path, std::string& error) const
{
    if (!contains(source)) {
        error = "the item being moved is outside the manuscript";
        return false;
    }
    if (path_is_within(source, manuscript_path())) {
        error = "the manuscript folder cannot be moved";
        return false;
    }
    if (!contains(destination_parent)) {
        error = "target folder is outside the manuscript";
        return false;
    }

    std::error_code code;
    if (!fs::is_directory(destination_parent, code)) {
        error = "target is not a folder";
        return false;
    }
    /* Moving a folder inside itself would detach the subtree from the
     * manuscript entirely, and rename() will not stop us. */
    if (path_is_within(source, destination_parent)) {
        error = "a folder cannot be moved into itself";
        return false;
    }

    const fs::path target = destination_parent / source.filename();
    if (paths_equal(target, source)) {
        moved_path = source;   // already there; nothing to do
        return true;
    }
    if (fs::exists(target, code)) {
        error = target.filename().string() + " already exists in the target folder";
        return false;
    }

    fs::rename(source, target, code);
    if (code) {
        error = "cannot move " + source.filename().string() + ": " + code.message();
        return false;
    }

    /* Tidiness rather than correctness: an entry left behind in the old folder
     * is already ignored, and the new folder would append the arrival anyway.
     * Doing it properly keeps the files saying what is actually there — and
     * stops an item that comes back later from silently reclaiming its old
     * position. */
    const std::string name = source.filename().string();
    forget_child(source.parent_path(), name);
    remember_child(destination_parent, name);

    moved_path = target;
    return true;
}

bool Project::move_entry_beside(const fs::path& source, const fs::path& anchor,
                                bool after, fs::path& moved_path,
                                std::string& error) const
{
    if (!contains(anchor)) {
        error = "the item being dropped onto is outside the manuscript";
        return false;
    }
    if (paths_equal(source, anchor)) {
        moved_path = source;   // dropped on itself
        return true;
    }

    const fs::path    parent      = anchor.parent_path();
    const std::string anchor_name = anchor.filename().string();

    if (!move_entry(source, parent, moved_path, error)) {
        return false;
    }

    /* Take the order from the binder rather than from the sidecar, so that a
     * folder nobody has arranged yet gets its alphabetical order written down
     * as it stands, with the new arrival slotted in. Otherwise the first drag
     * into such a folder would record two names and leave everything else to
     * pile up behind them. */
    std::vector<std::string> order = ordered_child_names(parent);
    const std::string moved_name = moved_path.filename().string();
    order.erase(std::remove(order.begin(), order.end(), moved_name), order.end());

    const auto at = std::find(order.begin(), order.end(), anchor_name);
    if (at == order.end()) {
        order.push_back(moved_name);   // the anchor went away under us
    } else {
        order.insert(after ? at + 1 : at, moved_name);
    }

    return write_child_order(parent, order, error);
}

bool Project::rename_entry(const fs::path& item, std::string_view new_name,
                           fs::path& renamed_path, std::string& error) const
{
    if (!contains(item)) {
        error = "the item being renamed is outside the manuscript";
        return false;
    }
    if (paths_equal(item, manuscript_path())) {
        error = "the manuscript folder cannot be renamed";
        return false;
    }

    std::error_code code;
    if (!fs::exists(item, code)) {
        error = item.filename().string() + " is no longer there";
        return false;
    }

    /* A folder is named outright; a document keeps the extension that makes it a
     * document, whatever the author typed. Taking the extension from the file
     * rather than assuming DOCUMENT_EXTENSION keeps a `.MD` on disk as it is. */
    const bool is_folder = fs::is_directory(item, code);
    const std::string base = sanitize_name(new_name);
    const fs::path target =
        item.parent_path() / (is_folder ? base : base + item.extension().string());

    if (paths_equal(target, item)) {
        renamed_path = item;   // the name it already has; nothing to do
        return true;
    }
    if (fs::exists(target, code)) {
        error = target.filename().string() + " already exists";
        return false;
    }

    fs::rename(item, target, code);
    if (code) {
        error = "cannot rename " + item.filename().string() + ": " + code.message();
        return false;
    }

    rename_child(item.parent_path(), item.filename().string(),
                 target.filename().string());

    renamed_path = target;
    return true;
}

bool Project::trash_entry(const fs::path& item, fs::path& trashed_path,
                          std::string& error) const
{
    if (!contains(item)) {
        error = "the item being deleted is outside the manuscript";
        return false;
    }
    if (paths_equal(item, manuscript_path())) {
        error = "the manuscript folder cannot be deleted";
        return false;
    }

    std::error_code code;
    if (!fs::exists(item, code)) {
        error = item.filename().string() + " is no longer there";
        return false;
    }
    const bool is_folder = fs::is_directory(item, code);

    /* The trash mirrors the project, so where the item sat is where it goes. */
    const fs::path relative = fs::relative(item, root_, code);
    if (code || relative.empty()) {
        error = "cannot work out where " + item.filename().string()
            + " sits in the project";
        return false;
    }

    const fs::path destination =
        root_ / PRIVATE_FOLDER_NAME / TRASH_FOLDER_NAME / relative.parent_path();
    fs::create_directories(destination, code);
    if (code) {
        error = "cannot make room in the trash: " + code.message();
        return false;
    }

    const fs::path target =
        unused_path(destination, is_folder ? item.filename().string()
                                           : item.stem().string(),
                    is_folder ? std::string() : item.extension().string());

    fs::rename(item, target, code);
    if (code) {
        error = "cannot delete " + item.filename().string() + ": " + code.message();
        return false;
    }

    forget_child(item.parent_path(), item.filename().string());

    trashed_path = target;
    return true;
}

bool Project::create_document(const fs::path& parent, std::string_view name,
                              fs::path& created_path, std::string& error) const
{
    if (!contains(parent)) {
        error = "target folder is outside the manuscript";
        return false;
    }

    const fs::path target = parent / (sanitize_name(name) + DOCUMENT_EXTENSION);
    std::error_code code;
    if (fs::exists(target, code)) {
        error = target.filename().string() + " already exists";
        return false;
    }
    if (!write_document(target, std::string_view(), error)) {
        return false;
    }
    created_path = target;
    return true;
}

/* ── creating without a name ────────────────────────────────────────────── */

/* All three take the free name and use it in the same call. Handing one back for
 * the caller to pass to `create_document` would leave a gap between choosing a
 * name and taking it, and would put a create that fails on a collision back in
 * the caller's way. */

bool Project::create_untitled_document(const fs::path& parent, fs::path& created_path,
                                       std::string& error) const
{
    if (!contains(parent)) {
        error = "target folder is outside the manuscript";
        return false;
    }

    const fs::path free = unused_path(parent, UNTITLED_NAME, DOCUMENT_EXTENSION);
    return create_document(parent, free.stem().string(), created_path, error);
}

bool Project::create_untitled_folder(const fs::path& parent, fs::path& created_path,
                                     std::string& error) const
{
    if (!contains(parent)) {
        error = "target folder is outside the manuscript";
        return false;
    }

    const fs::path free = unused_path(parent, UNTITLED_NAME, "");
    return create_folder(parent, free.filename().string(), created_path, error);
}

bool Project::group_into_untitled_folder(const fs::path& item, fs::path& folder_path,
                                         fs::path& moved_path,
                                         std::string& error) const
{
    if (!contains(item)) {
        error = "the item being grouped is outside the manuscript";
        return false;
    }

    const fs::path free = unused_path(item.parent_path(), UNTITLED_NAME, "");
    return group_into_new_folder(item, free.filename().string(), folder_path,
                                 moved_path, error);
}

/* ── documents ──────────────────────────────────────────────────────────── */

bool read_document(const fs::path& path, std::string& out, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

bool write_document(const fs::path& path, std::string_view markdown,
                    std::string& error)
{
    /* Before the write, and deliberately without checking the result: see
     * `capture_snapshot`, which never blocks the save it precedes. */
    capture_snapshot(path);
    return write_file_atomically(path, markdown, error);
}

std::string sanitize_name(std::string_view title)
{
    std::string out;
    bool pending_separator = false;

    for (unsigned char ch : title) {
        const bool space = ch == ' ' || ch == '\t';
        const bool safe = space || std::isalnum(ch) != 0 || ch == '-'
            || ch == '_' || ch == '\'' || ch >= 0x80;
        if (!safe) {
            pending_separator = true;
            continue;
        }

        if (space) {
            /* A space is a separator the author typed, so it stands in for one
             * we were about to invent: "a / b" is "a b" and not "a - b". */
            pending_separator = false;
            if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
            continue;
        }

        if (pending_separator && !out.empty() && out.back() != ' ') {
            out += '-';
        }
        pending_separator = false;
        out += static_cast<char>(ch);
    }

    /* Leading/trailing hyphens and spaces read badly — and a trailing space
     * would sit between the name and its extension — while a leading dot would
     * hide the file. */
    while (!out.empty() && (out.front() == '-' || out.front() == '.'
                            || out.front() == ' ')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == '-' || out.back() == ' ')) {
        out.pop_back();
    }

    return out.empty() ? "untitled" : out;
}

} // namespace wordsmith
