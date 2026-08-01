#include "session.hpp"

#include "safe-write.hpp"

#include <argo/argo.hpp>
#include <argo/exceptions.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace wordsmith {

namespace {

/* Bumped when the on-disk shape changes in a way older builds cannot read. A
 * version we do not understand is not refused, for the reason the header
 * gives. */
constexpr std::int64_t SESSION_FORMAT_VERSION = 1;

constexpr const char* STATE_SUBDIRECTORY = "wordsmith";
constexpr const char* SESSION_FILE_NAME = "session.json";

constexpr const char* VERSION_KEY = "version";
constexpr const char* PROJECTS_KEY = "projects";
constexpr const char* ROOT_KEY = "root";
constexpr const char* OPEN_DOCUMENT_KEY = "open-document";
constexpr const char* EXPANDED_KEY = "expanded";
constexpr const char* BINDER_VISIBLE_KEY = "binder-visible";
constexpr const char* INSPECTOR_VISIBLE_KEY = "inspector-visible";
constexpr const char* FORMAT_BAR_VISIBLE_KEY = "format-bar-visible";

/* An environment variable's value, or empty when it is unset or blank. XDG
 * treats a set-but-empty variable as unset, and so does this. */
std::string environment(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

/* The string at `key`, or empty when the field is absent or is something other
 * than a string. */
std::string string_field(const std::map<std::string, argo::json>& fields,
                         const char* key)
{
    auto found = fields.find(key);
    if (found == fields.end() || !found->second.is_string()) {
        return {};
    }
    return found->second.get_string();
}

/* The boolean at `key`, or `fallback` when the field is absent or is something
 * other than a boolean. A hand-edited `"yes"` reads as the default rather than
 * as true, the same way a malformed file reads as no saved state at all. */
bool bool_field(const std::map<std::string, argo::json>& fields, const char* key,
                bool fallback)
{
    auto found = fields.find(key);
    if (found == fields.end() || !found->second.is_boolean()) {
        return fallback;
    }
    return found->second.get_bool();
}

/* One entry, or nothing when it is not an object or names no project. */
bool read_entry(const argo::json& value, ProjectSession& out)
{
    if (!value.is_object()) {
        return false;
    }

    const std::map<std::string, argo::json>& fields = value.get_object();
    out.root = string_field(fields, ROOT_KEY);
    if (out.root.empty()) {
        return false;
    }
    out.open_document = string_field(fields, OPEN_DOCUMENT_KEY);
    out.binder_visible = bool_field(fields, BINDER_VISIBLE_KEY, true);
    out.inspector_visible = bool_field(fields, INSPECTOR_VISIBLE_KEY, true);
    out.format_bar_visible = bool_field(fields, FORMAT_BAR_VISIBLE_KEY, true);

    auto expanded = fields.find(EXPANDED_KEY);
    if (expanded != fields.end() && expanded->second.is_array()) {
        for (const argo::json& folder : expanded->second.get_array()) {
            if (folder.is_string() && !folder.get_string().empty()) {
                out.expanded.push_back(folder.get_string());
            }
        }
    }
    return true;
}

argo::json write_entry(const ProjectSession& session)
{
    argo::json expanded = argo::json::array({});
    for (const std::string& folder : session.expanded) {
        expanded.get_array().push_back(argo::json::string(folder));
    }

    return argo::json::object({
        {ROOT_KEY, argo::json::string(session.root)},
        {OPEN_DOCUMENT_KEY, argo::json::string(session.open_document)},
        {EXPANDED_KEY, expanded},
        {BINDER_VISIBLE_KEY, argo::json::boolean_value(session.binder_visible)},
        {INSPECTOR_VISIBLE_KEY, argo::json::boolean_value(session.inspector_visible)},
        {FORMAT_BAR_VISIBLE_KEY, argo::json::boolean_value(session.format_bar_visible)},
    });
}

} // namespace

fs::path session_path()
{
    const std::string state_home = environment("XDG_STATE_HOME");
    if (!state_home.empty()) {
        return fs::path(state_home) / STATE_SUBDIRECTORY / SESSION_FILE_NAME;
    }

    const std::string home = environment("HOME");
    const fs::path base = home.empty() ? fs::path(".") : fs::path(home) / ".local/state";
    return base / STATE_SUBDIRECTORY / SESSION_FILE_NAME;
}

fs::path session_key(const fs::path& root)
{
    if (root.empty()) {
        return root;
    }

    std::error_code code;
    fs::path key = fs::weakly_canonical(root, code);
    if (code || key.empty()) {
        key = fs::absolute(root, code);
        if (code) {
            return root;
        }
    }

    /* A trailing separator leaves an empty final component, which would make
     * `/books/novel/` a different key from `/books/novel`. */
    if (key.filename().empty() && key.has_parent_path()) {
        key = key.parent_path();
    }
    return key;
}

std::string session_relative(const fs::path& root, const fs::path& item)
{
    if (item.empty() || root.empty()) {
        return {};
    }

    const fs::path relative = session_key(item).lexically_relative(session_key(root));
    if (relative.empty()) {
        return {};
    }

    const std::string text = relative.generic_string();
    if (text == "." || text == ".." || text.rfind("../", 0) == 0) {
        return {};
    }
    return text;
}

fs::path session_absolute(const fs::path& root, const std::string& relative)
{
    if (relative.empty()) {
        return {};
    }
    return root / fs::path(relative);
}

std::vector<ProjectSession> load_session(const fs::path& file)
{
    std::vector<ProjectSession> sessions;

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return sessions;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    argo::json document;
    try {
        document = argo::json::parse(buffer.str());
    } catch (const argo::json_error&) {
        return sessions;
    }
    if (!document.is_object()) {
        return sessions;
    }

    const std::map<std::string, argo::json>& fields = document.get_object();
    auto projects = fields.find(PROJECTS_KEY);
    if (projects == fields.end() || !projects->second.is_array()) {
        return sessions;
    }

    for (const argo::json& entry : projects->second.get_array()) {
        ProjectSession session;
        if (read_entry(entry, session)) {
            sessions.push_back(std::move(session));
        }
    }
    return sessions;
}

ProjectSession session_for(const fs::path& file, const fs::path& root)
{
    ProjectSession result;
    result.root = session_key(root).string();

    const std::string key = result.root;
    for (const ProjectSession& saved : load_session(file)) {
        if (session_key(saved.root) != fs::path(key)) {
            continue;
        }

        /* Not claims about the filesystem, so there is nothing to check them
         * against and they carry over as written. */
        result.binder_visible = saved.binder_visible;
        result.inspector_visible = saved.inspector_visible;
        result.format_bar_visible = saved.format_bar_visible;

        /* Everything below is a hint about the filesystem, so the filesystem
         * gets the last word. A document that has been deleted or a folder that
         * has become a file is simply not restored. */
        std::error_code code;
        const fs::path document = session_absolute(root, saved.open_document);
        if (!document.empty() && fs::is_regular_file(document, code)) {
            result.open_document = saved.open_document;
        }
        for (const std::string& folder : saved.expanded) {
            if (fs::is_directory(session_absolute(root, folder), code)) {
                result.expanded.push_back(folder);
            }
        }
        break;
    }
    return result;
}

bool save_project_session(const ProjectSession& session, const fs::path& file,
                          std::string& error)
{
    const fs::path directory = file.parent_path();
    if (!directory.empty()) {
        std::error_code code;
        fs::create_directories(directory, code);
        if (code) {
            error = "cannot create " + directory.string() + ": " + code.message();
            return false;
        }
    }

    ProjectSession front = session;
    front.root = session_key(session.root).string();

    std::vector<ProjectSession> sessions = load_session(file);
    argo::json projects = argo::json::array({});
    projects.get_array().push_back(write_entry(front));

    for (const ProjectSession& saved : sessions) {
        if (projects.get_array().size() >= SESSION_PROJECT_LIMIT) {
            break;
        }
        if (session_key(saved.root) == fs::path(front.root)) {
            continue;   /* superseded by the entry just written */
        }
        projects.get_array().push_back(write_entry(saved));
    }

    argo::json document = argo::json::object({
        {VERSION_KEY, argo::json::integer_value(SESSION_FORMAT_VERSION)},
        {PROJECTS_KEY, projects},
    });

    return write_file_atomically(file, document.serialize() + "\n", error);
}

} // namespace wordsmith
