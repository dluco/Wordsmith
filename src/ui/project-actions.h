#pragma once

#include "inspector-panel.h"
#include "ui-state.h"

/* The verbs behind the File and Insert menus. Kept apart from menu-bar.c so
 * the action handlers stay thin and the dialog flows are in one place. */

void project_actions_new_dialog(WordsmithUiState* state);
void project_actions_open_dialog(WordsmithUiState* state);
void project_actions_open_path(WordsmithUiState* state, const char* root);
void project_actions_close(WordsmithUiState* state);

void project_actions_save(WordsmithUiState* state);

/** Prompt for a name, then create a folder in the binder's target folder. */
void project_actions_new_folder(WordsmithUiState* state);

/** Prompt for a title, then create a document and open it. */
void project_actions_new_text(WordsmithUiState* state);

/* The same two, but told where to put the new item rather than asking the
 * binder's selection. The context menu names its target at click time, since
 * the prompt outlives the popover that raised it. */
void project_actions_new_folder_in(WordsmithUiState* state, const char* parent);
void project_actions_new_text_in(WordsmithUiState* state, const char* parent);

/** Prompt for a name, then create a folder beside `item` and move `item` into
 *  it. Reopens the document afterwards if the move took it, or the folder it
 *  was in, out from under the editor. */
void project_actions_new_folder_with_selection(WordsmithUiState* state,
                                               const char* item);

/** Open `path` in the editor, saving the current document first. */
void project_actions_open_document(WordsmithUiState* state, const char* path);

/** Write one field the inspector committed back to the file behind it: a
 *  document's frontmatter, or a folder's sidecar, creating the sidecar if there
 *  is none.
 *
 *  This is the verb rather than something the inspector does itself because the
 *  document may also be open in the editor, which keeps the frontmatter bytes
 *  aside and puts them back on save. The buffer therefore has to be committed
 *  before the rewrite and told about it afterwards, and both ends of that live
 *  here. */
void project_actions_set_metadata(WordsmithUiState* state,
                                  const InspectorEdit* edit);

/** Put one recorded metadata edit back, or forward again. Undo lands here
 *  rather than in the editor because the file behind a field is not the buffer:
 *  it may be a folder's sidecar, and even for a document it is the frontmatter
 *  the editor deliberately does not hold. The write goes through the same path
 *  a typed edit does, so the ordering against the editor's stale prologue is
 *  handled once. */
void project_actions_apply_metadata_record(WordsmithUiState* state,
                                           const UndoRecord* record,
                                           gboolean reverse);

/** Open an entry over the name of the item at `path` in the binder. The verb
 *  rather than something the menu calls on the panel directly, so that F2 and
 *  the binder's context menu arrive by the same door every other item command
 *  does. */
void project_actions_rename(WordsmithUiState* state, const char* path);

/** Rename the item at `path`, which the binder has just had typed into it. Like
 *  a move, this takes the open document's path out from under the editor, so it
 *  saves first and reopens the document where it lands. */
void project_actions_rename_to(WordsmithUiState* state, const char* path,
                               const char* new_name);

/** Ask, and then move the item at `path` into the project's trash.
 *
 *  Asking first, even though nothing is destroyed: the trash is a folder the
 *  author has to go and find, and a chapter that leaves the binder without a
 *  word is indistinguishable from one that was lost. Deleting is also the one
 *  item command with no gesture to undo it — file operations are not on the undo
 *  stack (see undo-stack.h) — so the question is the only chance to say no. */
void project_actions_trash(WordsmithUiState* state, const char* path);

/* The two halves of a binder drag. Both save first, since a move takes the
 * open document's path out from under the editor, and both leave the moved
 * item selected. */

/** Move `source` into the folder `folder`, at the end of it. */
void project_actions_move_into(WordsmithUiState* state, const char* source,
                               const char* folder);

/** Move `source` alongside `anchor`, just before it or just after it. */
void project_actions_move_beside(WordsmithUiState* state, const char* source,
                                 const char* anchor, int after);
