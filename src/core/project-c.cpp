#include "project-c.h"

#include "markup-c.h"
#include "project.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace fs = std::filesystem;

using wordsworth::BinderEntry;

/* The C handles are the C++ types. Path strings are cached alongside the
 * project so the borrowed `const char*` accessors have somewhere stable to
 * point. */
struct WordsworthProject {
    std::unique_ptr<wordsworth::Project> project;
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

const BinderEntry* as_entry(const WordsworthBinderNode* node)
{
    return reinterpret_cast<const BinderEntry*>(node);
}

const WordsworthBinderNode* as_node(const BinderEntry* entry)
{
    return reinterpret_cast<const WordsworthBinderNode*>(entry);
}

} // namespace

/* ── lifecycle ──────────────────────────────────────────────────────────── */

WordsworthProject* wordsworth_project_open(const char* root, char** error)
{
    if (root == nullptr) {
        set_error(error, "no project path given");
        return nullptr;
    }

    std::string message;
    auto opened = wordsworth::Project::open(fs::path(root), message);
    if (opened == nullptr) {
        set_error(error, message);
        return nullptr;
    }

    auto* handle = new (std::nothrow) WordsworthProject();
    if (handle == nullptr) {
        return nullptr;
    }
    handle->project = std::move(opened);
    handle->refresh_caches();
    return handle;
}

WordsworthProject* wordsworth_project_create(const char* root, const char* title,
                                             char** error)
{
    if (root == nullptr) {
        set_error(error, "no project path given");
        return nullptr;
    }

    std::string message;
    auto created = wordsworth::Project::create(
        fs::path(root), title != nullptr ? title : "", message);
    if (created == nullptr) {
        set_error(error, message);
        return nullptr;
    }

    auto* handle = new (std::nothrow) WordsworthProject();
    if (handle == nullptr) {
        return nullptr;
    }
    handle->project = std::move(created);
    handle->refresh_caches();
    return handle;
}

void wordsworth_project_free(WordsworthProject* project)
{
    delete project;
}

/* ── accessors ──────────────────────────────────────────────────────────── */

const char* wordsworth_project_title(const WordsworthProject* project)
{
    return project != nullptr ? project->project->title().c_str() : "";
}

const char* wordsworth_project_root(const WordsworthProject* project)
{
    return project != nullptr ? project->root_cache.c_str() : "";
}

const char* wordsworth_project_manuscript_path(const WordsworthProject* project)
{
    return project != nullptr ? project->manuscript_cache.c_str() : "";
}

void wordsworth_project_reload(WordsworthProject* project)
{
    if (project == nullptr) {
        return;
    }
    project->project->reload_binder();
}

const WordsworthBinderNode* wordsworth_project_binder_root(
    const WordsworthProject* project)
{
    if (project == nullptr) {
        return nullptr;
    }
    return as_node(&project->project->binder());
}

/* ── binder nodes ───────────────────────────────────────────────────────── */

const char* wordsworth_binder_node_name(const WordsworthBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr ? entry->name.c_str() : "";
}

const char* wordsworth_binder_node_path(const WordsworthBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr ? entry->path_string.c_str() : "";
}

int wordsworth_binder_node_is_folder(const WordsworthBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr && entry->is_folder ? 1 : 0;
}

size_t wordsworth_binder_node_child_count(const WordsworthBinderNode* node)
{
    const BinderEntry* entry = as_entry(node);
    return entry != nullptr ? entry->children.size() : 0;
}

const WordsworthBinderNode* wordsworth_binder_node_child(
    const WordsworthBinderNode* node, size_t index)
{
    const BinderEntry* entry = as_entry(node);
    if (entry == nullptr || index >= entry->children.size()) {
        return nullptr;
    }
    return as_node(&entry->children[index]);
}

/* ── mutation ───────────────────────────────────────────────────────────── */

int wordsworth_project_create_folder(WordsworthProject* project,
                                     const char* parent_path, const char* name,
                                     char** error)
{
    if (project == nullptr || parent_path == nullptr || name == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    std::string message;
    if (!project->project->create_folder(fs::path(parent_path), name, message)) {
        set_error(error, message);
        return 0;
    }
    return 1;
}

int wordsworth_project_create_document(WordsworthProject* project,
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

/* ── documents ──────────────────────────────────────────────────────────── */

char* wordsworth_document_read(const char* path, char** error)
{
    if (path == nullptr) {
        set_error(error, "no document path given");
        return nullptr;
    }

    std::string markdown;
    std::string message;
    if (!wordsworth::read_document(fs::path(path), markdown, message)) {
        set_error(error, message);
        return nullptr;
    }
    return duplicate(markdown);
}

int wordsworth_document_write(const char* path, const char* markdown, char** error)
{
    if (path == nullptr || markdown == nullptr) {
        set_error(error, "missing argument");
        return 0;
    }

    std::string message;
    if (!wordsworth::write_document(fs::path(path), markdown, message)) {
        set_error(error, message);
        return 0;
    }
    return 1;
}
