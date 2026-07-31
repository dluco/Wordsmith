#include "project.hpp"

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

} // namespace

/* ── binder ─────────────────────────────────────────────────────────────── */

BinderEntry load_binder(const fs::path& manuscript_root)
{
    BinderEntry root;
    root.name        = manuscript_root.filename().string();
    root.path        = manuscript_root;
    root.path_string = manuscript_root.string();
    root.is_folder   = true;

    std::error_code code;
    if (!fs::is_directory(manuscript_root, code)) {
        return root;
    }

    for (const fs::directory_entry& entry :
         fs::directory_iterator(manuscript_root, code)) {
        const fs::path& path = entry.path();
        // Skip dotfiles: editor cruft and, later, our own sidecars.
        if (!path.filename().empty() && path.filename().string()[0] == '.') {
            continue;
        }

        if (entry.is_directory(code)) {
            root.children.push_back(load_binder(path));
        } else if (entry.is_regular_file(code) && is_document(path)) {
            BinderEntry document;
            document.name        = path.stem().string();
            document.path        = path;
            document.path_string = path.string();
            root.children.push_back(std::move(document));
        }
    }

    std::sort(root.children.begin(), root.children.end(), binder_order);
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

    const fs::path project_file = root_ / PROJECT_FILE_NAME;
    std::ofstream stream(project_file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write " + project_file.string();
        return false;
    }
    stream << document.serialize() << '\n';
    if (!stream) {
        error = "failed writing " + project_file.string();
        return false;
    }
    return true;
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
    if (path_is_within(target, source) && path_is_within(source, target)) {
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
    moved_path = target;
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
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write " + path.string();
        return false;
    }
    stream.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
    if (!stream) {
        error = "failed writing " + path.string();
        return false;
    }
    return true;
}

std::string sanitize_name(std::string_view title)
{
    std::string out;
    bool pending_separator = false;

    for (unsigned char ch : title) {
        const bool safe = std::isalnum(ch) != 0 || ch == '-' || ch == '_'
            || ch == '\'' || ch >= 0x80;
        if (safe) {
            if (pending_separator && !out.empty()) {
                out += '-';
            }
            pending_separator = false;
            out += static_cast<char>(ch);
        } else {
            pending_separator = true;
        }
    }

    // Leading/trailing hyphens read badly and a leading dot would hide the file.
    while (!out.empty() && (out.front() == '-' || out.front() == '.')) {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }

    return out.empty() ? "untitled" : out;
}

} // namespace wordsmith
