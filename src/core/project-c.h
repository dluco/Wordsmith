#ifndef WORDSWORTH_PROJECT_C_H
#define WORDSWORTH_PROJECT_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C bridge over wordsworth::Project. Every call that can fail takes a trailing
 * `char** error`; on failure it is set to a message the caller owns and frees
 * with wordsworth_free_string() (declared in markup-c.h). Pass NULL to ignore
 * the message. On success the out-parameter is left untouched. */

typedef struct WordsworthProject    WordsworthProject;
typedef struct WordsworthBinderNode WordsworthBinderNode;

/* ── lifecycle ──────────────────────────────────────────────────────────── */

/** Open the project directory at `root`. NULL on failure. */
WordsworthProject* wordsworth_project_open(const char* root, char** error);

/** Create a project directory at `root` with a manuscript folder inside.
 *  Fails if one is already there. NULL on failure. */
WordsworthProject* wordsworth_project_create(const char* root, const char* title,
                                             char** error);

void wordsworth_project_free(WordsworthProject* project);

/* ── accessors ──────────────────────────────────────────────────────────── */

/* Borrowed, valid until the project is freed or reloaded. */
const char* wordsworth_project_title(const WordsworthProject* project);
const char* wordsworth_project_root(const WordsworthProject* project);
const char* wordsworth_project_manuscript_path(const WordsworthProject* project);

/** Rescan the manuscript folder. Invalidates every binder node handle and
 *  every borrowed string from them. */
void wordsworth_project_reload(WordsworthProject* project);

/** The synthetic root standing for the manuscript folder. Never NULL for a
 *  live project. */
const WordsworthBinderNode* wordsworth_project_binder_root(
    const WordsworthProject* project);

/* ── binder nodes ───────────────────────────────────────────────────────── */

/* Handles point into the project's tree and stay valid until the next
 * wordsworth_project_reload() or the project is freed. */

const char* wordsworth_binder_node_name(const WordsworthBinderNode* node);
const char* wordsworth_binder_node_path(const WordsworthBinderNode* node);
int         wordsworth_binder_node_is_folder(const WordsworthBinderNode* node);
size_t      wordsworth_binder_node_child_count(const WordsworthBinderNode* node);

const WordsworthBinderNode* wordsworth_binder_node_child(
    const WordsworthBinderNode* node, size_t index);

/* ── mutation ───────────────────────────────────────────────────────────── */

/* `parent_path` must name the manuscript folder or a folder inside it, and
 * `name` is a user-typed title that gets sanitised into a filename. Neither
 * call rescans: follow with wordsworth_project_reload(). */

int wordsworth_project_create_folder(WordsworthProject* project,
                                     const char* parent_path, const char* name,
                                     char** error);

/** On success `created_path`, if non-NULL, receives the new document's path;
 *  the caller frees it with wordsworth_free_string(). */
int wordsworth_project_create_document(WordsworthProject* project,
                                       const char* parent_path, const char* name,
                                       char** created_path, char** error);

/* ── documents ──────────────────────────────────────────────────────────── */

/** Read a document's Markdown. Caller frees the result with
 *  wordsworth_free_string(). NULL on failure. */
char* wordsworth_document_read(const char* path, char** error);

int wordsworth_document_write(const char* path, const char* markdown, char** error);

#ifdef __cplusplus
}
#endif

#endif /* WORDSWORTH_PROJECT_C_H */
