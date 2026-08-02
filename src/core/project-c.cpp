#include "project-c.h"

#include "markup-c.h"
#include "project.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace fs = std::filesystem;

using wordsmith::BinderEntry;

/* The C handles are the C++ types. Path strings are cached alongside the
 * project so the borrowed `const char*` accessors have somewhere stable to
 * point. */
struct WordsmithProject {
    std::unique_ptr<wordsmith::Project> project;
    std::string                          root_cache;
    std::string                          manuscript_cache;

    void refresh_caches()
    {
        root_cache       = project->root().string();
        manuscript_cache = project->manuscript_path().string();
    }
};

namespace {

char* duplicate(const std::string& text)
{
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

void set_error(char** error, const std::string& message)
{
    if (error != nullptr) {
        *error = duplicate(message);
    }
}

const BinderEntry* as_entry(const WordsmithBinderNode* node)
{
    return reinterpret_cast<const BinderEntry*>(node);
}

const WordsmithBinderNode* as_node(const BinderEntry* entry)
{
    return reinterpret_cast<const WordsmithBinderNode*>(entry);
}

} // namespace

/* ── lifecycle ──────────────────────────────────────────────────────────── */

WordsmithProject* wordsmith_project_open(const char* root, char** error)
{
    if (root == nullptr) {
        set_error(error, "no project path given");
        return nullptr;
    }

    std::string message;
    auto opened = wordsmith::Project::open(fs::path(root), message);
    if (opened == nullptr) {
        set_error(error, message);
        return nullptr;
    }

    auto* handle = new (std::nothrow) WordsmithProject();
    if (handle == nullptr) {
        return nullptr;
    }
    handle->project = std::move(opened);
    handle->refresh_caches();
    return handle;
}

WordsmithProject* wordsmith_project_create(const char* root, const char* title,
                                             char** error)
{
    if (root == nullptr) {
        set_error(error, "no project path given");
        return nullptr;
    }

    std::string message;
    auto created = wordsmith::Project::create(
        fs::path(root), title != nullptr ? title : "", message);
    if (created == nullptr) {
        set_error(error, message);
        return nullptr;
    }

    auto* handle = new (std::nothrow) WordsmithProject();
    if (handle == nullptr) {
        return nullptr;
    }
    handle->project = std::move(created);
    handle->refresh_caches();
    return handle;
}

void wordsmith_project_free(WordsmithProject* project)
{
    delete project;
}

/* ── accessors ──────────────────────────────────────────────────────────── */

const char* wordsmith_project_title(const WordsmithProject* project)
{
    return project != nullptr ? project->project->title().c_str() : "";
}

const char* wordsmith_project_root(const WordsmithProject* project)
{
    return project != nullptr ? project->root_cache.c_str() : "";
}

const char* wordsmith_project_manuscript_path(const WordsmithProject* project)
{
    return project != nullptr ? project->manuscript_cache.c_str() : "";
}

void wordsmith_project_reload(WordsmithProject* project)
{
    if (project == nullptr) {
        return;
    }
    project->project->reload_binder();
}

const WordsmithBinderNode* wordsmith_project_binder_root(
    const WordsmithProject* project)
{
    if (project == nullptr) {
        return nullptr;
    }
    return as_node(&project->project->binder());
}

/* ── binder nodes ───────────────────────────────────────────────────────── */

const char* wordsmith_binder_node_name(const WordsmithBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr ? entry->name.c_str() : "";
}

const char* wordsmith_binder_node_path(const WordsmithBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr ? entry->path_string.c_str() : "";
}

int wordsmith_binder_node_is_folder(const WordsmithBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr && entry->is_folder ? 1 : 0;
}

size_t wordsmith_binder_node_child_count(const WordsmithBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr ? entry->children.size() : 0;
}

const WordsmithBinderNode* wordsmith_binder_node_child(
    const WordsmithBinderNode* node, size_t index)
{
    const BinderEntry* entry = as_entry(node);
    if (entry == nullptr || index >= entry->children.size()) {
        return nullptr;
    }
    return as_node(&entry->children[index]);
}

/* ── mutation ───────────────────────────────────────────────────────────── */

int wordsmith_project_create_folder(WordsmithProject* project,
                                     const char* parent_path, const char* name,
                                     char** created_path, char** error)
{
    if (project == nullptr || parent_path == nullptr || name == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    fs::path created;
    std::string message;
    if (!project->project->create_folder(fs::path(parent_path), name, created,
                                         message)) {
        set_error(error, message);
        return 0;
    }
    if (created_path != nullptr) {
        *created_path = duplicate(created.string());
    }
    return 1;
}

int wordsmith_project_create_document(WordsmithProject* project,
                                       const char* parent_path, const char* name,
                                       char** created_path, char** error)
{
    if (project == nullptr || parent_path == nullptr || name == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    fs::path created;
    std::string message;
    if (!project->project->create_document(fs::path(parent_path), name, created,
                                           message)) {
        set_error(error, message);
        return 0;
    }
    if (created_path != nullptr) {
        *created_path = duplicate(created.string());
    }
    return 1;
}

int wordsmith_project_move(WordsmithProject* project, const char* source_path,
                            const char* parent_path, char** moved_path,
                            char** error)
{
    if (project == nullptr || source_path == nullptr || parent_path == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    fs::path moved;
    std::string message;
    if (!project->project->move_entry(fs::path(source_path), fs::path(parent_path),
                                      moved, message)) {
        set_error(error, message);
        return 0;
    }
    if (moved_path != nullptr) {
        *moved_path = duplicate(moved.string());
    }
    return 1;
}

int wordsmith_project_move_beside(WordsmithProject* project,
                                   const char* source_path, const char* anchor_path,
                                   int after, char** moved_path, char** error)
{
    if (project == nullptr || source_path == nullptr || anchor_path == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    fs::path moved;
    std::string message;
    if (!project->project->move_entry_beside(fs::path(source_path),
                                             fs::path(anchor_path), after != 0,
                                             moved, message)) {
        set_error(error, message);
        return 0;
    }
    if (moved_path != nullptr) {
        *moved_path = duplicate(moved.string());
    }
    return 1;
}

int wordsmith_project_group_into_new_folder(WordsmithProject* project,
                                             const char* item_path,
                                             const char* name, char** folder_path,
                                             char** moved_path, char** error)
{
    if (project == nullptr || item_path == nullptr || name == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    fs::path folder;
    fs::path moved;
    std::string message;
    if (!project->project->group_into_new_folder(fs::path(item_path), name, folder,
                                                 moved, message)) {
        set_error(error, message);
        return 0;
    }
    if (folder_path != nullptr) {
        *folder_path = duplicate(folder.string());
    }
    if (moved_path != nullptr) {
        *moved_path = duplicate(moved.string());
    }
    return 1;
}

int wordsmith_project_rename(WordsmithProject* project, const char* item_path,
                              const char* new_name, char** renamed_path,
                              char** error)
{
    if (project == nullptr || item_path == nullptr || new_name == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    fs::path renamed;
    std::string message;
    if (!project->project->rename_entry(fs::path(item_path), new_name, renamed,
                                        message)) {
        set_error(error, message);
        return 0;
    }
    if (renamed_path != nullptr) {
        *renamed_path = duplicate(renamed.string());
    }
    return 1;
}

int wordsmith_project_trash(WordsmithProject* project, const char* item_path,
                             char** trashed_path, char** error)
{
    if (project == nullptr || item_path == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    fs::path trashed;
    std::string message;
    if (!project->project->trash_entry(fs::path(item_path), trashed, message)) {
        set_error(error, message);
        return 0;
    }
    if (trashed_path != nullptr) {
        *trashed_path = duplicate(trashed.string());
    }
    return 1;
}

/* ── folder metadata ────────────────────────────────────────────────────── */

char* wordsmith_folder_metadata_path(const char* folder_path)
{
    if (folder_path == nullptr) {
        return nullptr;
    }
    return duplicate(wordsmith::folder_metadata_path(fs::path(folder_path)).string());
}

int wordsmith_project_set_child_order(WordsmithProject* project,
                                       const char* folder_path,
                                       const char* const* names, size_t count,
                                       char** error)
{
    if (project == nullptr || folder_path == nullptr || (names == nullptr && count > 0)) {
        set_error(error, "missing argument");
        return 0;
    }

    std::vector<std::string> order;
    order.reserve(count);
    for (size_t index = 0; index < count; index++) {
        if (names[index] != nullptr) {
            order.emplace_back(names[index]);
        }
    }

    std::string message;
    if (!project->project->set_child_order(fs::path(folder_path), order, message)) {
        set_error(error, message);
        return 0;
    }
    return 1;
}

/* ── documents ──────────────────────────────────────────────────────────── */

char* wordsmith_document_read(const char* path, char** error)
{
    if (path == nullptr) {
        set_error(error, "no document path given");
        return nullptr;
    }

    std::string markdown;
    std::string message;
    if (!wordsmith::read_document(fs::path(path), markdown, message)) {
        set_error(error, message);
        return nullptr;
    }
    return duplicate(markdown);
}

int wordsmith_document_write(const char* path, const char* markdown, char** error)
{
    if (path == nullptr || markdown == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    std::string message;
    if (!wordsmith::write_document(fs::path(path), markdown, message)) {
        set_error(error, message);
        return 0;
    }
    return 1;
}
