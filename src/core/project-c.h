#ifndef WORDSMITH_PROJECT_C_H
#define WORDSMITH_PROJECT_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C bridge over wordsmith::Project. Every call that can fail takes a trailing
 * `char** error`; on failure it is set to a message the caller owns and frees
 * with wordsmith_free_string() (declared in markup-c.h). Pass NULL to ignore
 * the message. On success the out-parameter is left untouched. */

typedef struct WordsmithProject    WordsmithProject;
typedef struct WordsmithBinderNode WordsmithBinderNode;

/* ── lifecycle ──────────────────────────────────────────────────────────── */

/** Open the project directory at `root`. NULL on failure. */
WordsmithProject* wordsmith_project_open(const char* root, char** error);

/** Create a project directory at `root` with a manuscript folder inside.
 *  Fails if one is already there. NULL on failure. */
WordsmithProject* wordsmith_project_create(const char* root, const char* title,
                                             char** error);

void wordsmith_project_free(WordsmithProject* project);

/* ── accessors ──────────────────────────────────────────────────────────── */

/* Borrowed, valid until the project is freed or reloaded. */
const char* wordsmith_project_title(const WordsmithProject* project);
const char* wordsmith_project_root(const WordsmithProject* project);
const char* wordsmith_project_manuscript_path(const WordsmithProject* project);

/** Rescan the manuscript folder. Invalidates every binder node handle and
 *  every borrowed string from them. */
void wordsmith_project_reload(WordsmithProject* project);

/** The synthetic root standing for the manuscript folder. Never NULL for a
 *  live project. */
const WordsmithBinderNode* wordsmith_project_binder_root(
    const WordsmithProject* project);

/* ── binder nodes ───────────────────────────────────────────────────────── */

/* Handles point into the project's tree and stay valid until the next
 * wordsmith_project_reload() or the project is freed. */

const char* wordsmith_binder_node_name(const WordsmithBinderNode* node);
const char* wordsmith_binder_node_path(const WordsmithBinderNode* node);
int         wordsmith_binder_node_is_folder(const WordsmithBinderNode* node);
size_t      wordsmith_binder_node_child_count(const WordsmithBinderNode* node);

const WordsmithBinderNode* wordsmith_binder_node_child(
    const WordsmithBinderNode* node, size_t index);

/* ── mutation ───────────────────────────────────────────────────────────── */

/* `parent_path` must name the manuscript folder or a folder inside it, and
 * `name` is a user-typed title that gets sanitised into a filename. Neither
 * call rescans: follow with wordsmith_project_reload(). */

/** On success `created_path`, if non-NULL, receives the new folder's path;
 *  the caller frees it with wordsmith_free_string(). */
int wordsmith_project_create_folder(WordsmithProject* project,
                                     const char* parent_path, const char* name,
                                     char** created_path, char** error);

/** On success `created_path`, if non-NULL, receives the new document's path;
 *  the caller frees it with wordsmith_free_string(). */
int wordsmith_project_create_document(WordsmithProject* project,
                                       const char* parent_path, const char* name,
                                       char** created_path, char** error);

/** Move the folder or document at `source_path` into `parent_path`, keeping
 *  its name. Both must be inside the manuscript, and a folder cannot be moved
 *  into itself. On success `moved_path`, if non-NULL, receives the new
 *  location. */
int wordsmith_project_move(WordsmithProject* project, const char* source_path,
                            const char* parent_path, char** moved_path,
                            char** error);

/** Move the item at `source_path` into `anchor_path`'s folder and place it just
 *  before `anchor_path`, or just after it when `after` is non-zero. Records the
 *  resulting order. On success `moved_path`, if non-NULL, receives the new
 *  location. */
int wordsmith_project_move_beside(WordsmithProject* project,
                                   const char* source_path, const char* anchor_path,
                                   int after, char** moved_path, char** error);

/** Create a folder called `name` beside `item_path` and move the item into it.
 *  `folder_path` and `moved_path`, when non-NULL, receive the new folder and
 *  the item's new location. */
int wordsmith_project_group_into_new_folder(WordsmithProject* project,
                                             const char* item_path,
                                             const char* name, char** folder_path,
                                             char** moved_path, char** error);

/* The same three, for an author who has not been asked for a name and is about
 * to type one into the binder. Each picks a name nothing in the target folder is
 * using — `Untitled`, then `Untitled-2` and so on — so none of them can fail on
 * a collision the caller had no way to see coming. */

int wordsmith_project_create_untitled_document(WordsmithProject* project,
                                                const char* parent_path,
                                                char** created_path, char** error);

int wordsmith_project_create_untitled_folder(WordsmithProject* project,
                                              const char* parent_path,
                                              char** created_path, char** error);

int wordsmith_project_group_into_untitled_folder(WordsmithProject* project,
                                                  const char* item_path,
                                                  char** folder_path,
                                                  char** moved_path, char** error);

/** Rename the item at `item_path` to `new_name`, a user-typed title that gets
 *  sanitised into a filename. The item keeps its place in its folder's recorded
 *  order. On success `renamed_path`, if non-NULL, receives the new location. */
int wordsmith_project_rename(WordsmithProject* project, const char* item_path,
                              const char* new_name, char** renamed_path,
                              char** error);

/** Move the item at `item_path` into the project's trash, under
 *  `.wordsmith/trash/`, where it stays until swept by hand. On success
 *  `trashed_path`, if non-NULL, receives where it landed. */
int wordsmith_project_trash(WordsmithProject* project, const char* item_path,
                             char** trashed_path, char** error);

/* ── folder metadata ────────────────────────────────────────────────────── */

/** Where `folder_path` keeps its sidecar of fields and child order. The file
 *  need not exist. Caller frees the result with wordsmith_free_string(). */
char* wordsmith_folder_metadata_path(const char* folder_path);

/** Record the order of a folder's children, named by filename. Leaves the rest
 *  of the sidecar alone, and creates it if there is none. */
int wordsmith_project_set_child_order(WordsmithProject* project,
                                       const char* folder_path,
                                       const char* const* names, size_t count,
                                       char** error);

/* ── documents ──────────────────────────────────────────────────────────── */

/** Read a document's Markdown. Caller frees the result with
 *  wordsmith_free_string(). NULL on failure. */
char* wordsmith_document_read(const char* path, char** error);

int wordsmith_document_write(const char* path, const char* markdown, char** error);

#ifdef __cplusplus
}
#endif

#endif /* WORDSMITH_PROJECT_C_H */
