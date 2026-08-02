# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

Dependencies: GTK4 ≥ 4.12 (via pkg-config), `glib-compile-resources`, CMake ≥ 3.31. The
`argo` JSON library is a git submodule — `git submodule update --init` before a
first build.

```sh
cmake --preset debug                 # configure into cmake-build-debug/
cmake --build --preset debug         # build everything
ctest --preset debug                 # run every suite
ctest --preset debug -R yaml         # run one suite by name
./cmake-build-debug/src/driver/wordsmith [project-dir]   # run the app
```

`--preset debug-asan` is the same build under AddressSanitizer.

Test executables are also runnable directly (e.g.
`./cmake-build-debug/tests/core/wordsmith-yaml-tests`), which is usually the
fastest way to iterate on a core change.

## Testing conventions

There is no test framework. Core tests are plain C++ programs: a file-local
`failures` counter, `check()`/`check_equal()` helpers that print `FAIL: <what>`,
and a `main` that calls each `test_*()` function and returns 1 if anything
failed. Add a new suite by dropping a file in `tests/core/` and adding one
`wordsmith_add_core_test(...)` line to `tests/core/CMakeLists.txt`.

UI tests use GLib's `g_test_*` and **must run headless** — there is no display in
CI or under `ctest`. `tests/ui/test-menu-bar.c` is deliberately thin for exactly
this reason: anything needing a realised widget has to be split behind a
display-free seam before it can be tested.

## Architecture

Three layers, and the boundaries between them are the load-bearing part of the
design:

- **`src/core/` — C++17, no GLib.** The model: project/binder (`project.cpp`),
  Markdown (`markup.cpp`), YAML and frontmatter (`yaml.cpp`, `frontmatter.cpp`).
  Nothing here knows about GTK, and it should stay that way — these are the
  files that are testable without a display.
- **`src/core/*-c.h` / `*-c.cpp` — the C bridge.** `extern "C"` opaque handles
  over the C++ classes. This is the only door between the two languages.
- **`src/ui/` — C17 + GTK4.** One file per pane, plus `ui-state.c` and
  `project-actions.c`.

`src/driver/` is a four-line `main` calling into `ui/wordsmith-ui.c`.

### Ownership across the bridge

Every fallible bridge call takes a trailing `char** error`; on failure it holds a
message the caller owns. Strings the core hands back (errors, document text,
created paths) are `malloc`'d and freed with **`wordsmith_free_string()`** from
`core/markup-c.h` — *not* `g_free`. Strings the UI allocates with GLib are freed
with `g_free`. Mixing the two is the easiest mistake to make here.

Accessors return **borrowed** pointers into the core's storage.
`wordsmith_project_reload()` invalidates every `WordsmithBinderNode*` and every
string borrowed from one, so the binder rebuilds its whole model rather than
patching it.

### The filesystem is the source of truth

A project is a directory holding `project.wordsmith` (JSON, parsed by argo) and a
manuscript folder of `.md` files. The JSON names the manuscript folder and holds
project-wide settings; **it does not enumerate documents**. The binder comes from
a directory scan.

Ordering lives in a per-folder `metadata.yaml` sidecar inside the folder it
describes, holding both the folder's own fields (it has no frontmatter of its
own) and a `children:` list. That list can only ever *reorder* what the scan
found — it cannot assert that something exists. Deleting a chapter outside
Wordsmith makes it vanish; adding one makes it appear at the end; a stale list
gives a mis-ordered binder, never a broken one.

`load_binder()` in `project.cpp` is the documented seam if this ever needs to
become a central index instead.

### Every write is atomic

Nothing truncates a file in place. `write_file_atomically()` in `safe-write.cpp`
writes to a hidden sibling temp file, fsyncs it, renames it over the target, and
fsyncs the directory; `write_document()` and `save_settings()` are both thin
wrappers over it, and **anything new that writes into the project must go
through it too**. The temp file is a sibling rather than one in `/tmp` because
`rename` is only atomic within a filesystem.

This guarantees a write lands whole. It does not guarantee the bytes were right
— editor save is a lossy round trip through `markup.hpp`, so a bug there commits
a well-formed wrong document. That is what snapshots are for.

### Snapshots

`write_document()` also calls `capture_snapshot()` (`snapshots.cpp`) first,
copying the previous contents into `.wordsmith/snapshots/` at the project root,
mirroring the project layout, five per document. `capture_snapshot` **never
blocks the save it precedes** — every failure and every skip returns false and
is ignored, because refusing to save the author's words to protect a backup of
their older words trades a certain loss for a possible one.

Two rules keep the ring useful, and both matter because one inspector
interaction writes a document several times: identical contents are not stored
twice, and neither is anything within ten minutes of the newest snapshot.
Without the cooldown, typing a synopsis would evict every version from before
the current sitting.

Snapshot filenames are `<UTC stamp>-<counter>.md` and their **lexicographic
order is the history**: `snapshots()` sorts by name and eviction drops from the
front. The counter therefore climbs past whatever exists rather than filling the
gap eviction just made — reusing a freed name reorders history and evicts the
wrong version next time.

### Preferences

Anything describing the person rather than the manuscript lives outside every
project, in `$XDG_CONFIG_HOME/wordsmith/settings.json` (`~/.config/...` when that
is unset). `preferences.cpp` reads and writes it, through the same
`write_file_atomically()` as everything else. A project directory travels, so
nothing about how one author likes to read may ride along inside it.

Reading that file never fails: missing, unreadable or malformed gives defaults,
and out-of-range values are clamped rather than refused. It is not the author's
work, and refusing to start over it would cost more than it saves. Saving
re-emits the whole file from the struct, so a key a newer build wrote is dropped
by an older one, the same trade `project.wordsmith` makes.

The editor's text size is the first such preference. It is applied the way the
stylesheet is, through one CSS provider on the display carrying a `font-size`
rule scoped to `.editor-pane` (`ui/text-scale.c`), rather than per panel: the
size is one answer for the whole application, restored before any window exists.
`text_scale_css()` is the display-free seam the UI test parses.

### Session state

Which binder folders were twisted open, which document was being edited, and
which side panes were on screen are remembered per project in
`$XDG_STATE_HOME/wordsmith/session.json` (`~/.local/state/...` when that is
unset). `session.cpp` reads and writes it.

Three files could have held this and two are wrong. Not `project.wordsmith`,
which describes the work: expanded rows would churn it on every click and give
two people sharing a project an argument about whose binder is open. Not
preferences' `settings.json` either, which is *config*, the deliberate answers
one author gives, in a file they are invited to edit; this is *state*, restored
without being asked for, and there is one entry per project rather than one for
the application. It stays outside the project for the reason preferences do: a
project directory travels.

One file holds every project, keyed by the normalised project root, most
recently opened first, capped at `SESSION_PROJECT_LIMIT`. One file rather than
one per project leaves nothing to collect when a project is deleted, and the cap
bounds the growth. Inside an entry, paths are relative to the root.

Reading never fails, the same as preferences. Beyond that, **a recorded path
with nothing behind it on disk is dropped rather than restored** (`session_for`).
That is the `children:` contract again: what is written down can only reopen
what the scan already found. A document deleted outside Wordsmith opens nothing
instead of raising an error on launch.

`binder-visible`, `inspector-visible` and `format-bar-visible` are the
remembered things that are not paths, so there is no filesystem to check them
against and they carry over as written. Every way of not saying one — no entry,
no key, a hand-edited value of the wrong type — defaults to **true**, because a
missing answer must never put away something the author did not ask to lose.
Anything non-path added here inherits that rule: pick the default that costs
nothing when it is wrong.

They cross the C bridge as a `WordsmithSessionPanes` struct rather than as three
`int` parameters. Adjacent flags of the same type are a swap nothing downstream
would catch — the author would just find the wrong thing missing — and the test
writes one shown per round, each round a different one, for that reason.

The UI side lives in `ui-state.c`: `ui_state_remember_session()` starts a
one-second timer so a run of expander clicks costs one write,
`ui_state_flush_session()` forces it out ahead of anything that takes the
project away, and `ui_state_set_project()` restores at the other end. Restoring
selects the saved row rather than loading it directly, so the ordinary
selection path opens the editor and inspector. `restoring_session` is what keeps
putting a view back from counting as a change to it. A failed write warns and is
otherwise ignored; a view that does not come back is not worth a dialog.

`ui_state_set_binder_visible()` and `ui_state_set_inspector_visible()` are the
only ways a side pane is shown or put away. The View menu's toggles call them
rather than moving the panes themselves, so restoring from the session moves the
check mark by the same code a click does and the two cannot drift apart. Their
chords are the shifted form of the format key sharing the letter — Shift+Ctrl+B
beside Ctrl+B for bold, Shift+Ctrl+I beside Ctrl+I for italic.

`binder_panel_reload()` also carries the expansion across a rebuild, through the
same two calls. Without it, creating a document would fold the whole binder
shut.

### Frontmatter: surgical, never lossy

`yaml.cpp` is a deliberately small YAML reader (no anchors, no flow mappings,
nesting stops at two levels). Its distinguishing feature is that every node
records **byte ranges** (`entry` and `value`) into the parsed text. That is what
lets `set_field()` / `set_sequence()` rewrite one field's bytes and preserve
everything else — field order, quoting, comments, blank lines, and the Markdown
body. Anything that writes metadata should go through those, read-modify-write,
rather than re-emitting a file (see `write_child_order`).

Frontmatter detection (`frontmatter::scan`) is structural, not a regex: a leading
`---` is also a valid CommonMark thematic break, and only the presence of a
closing fence disambiguates them.

The editor never holds frontmatter in its buffer. `editor_panel_load()` splits it
off and keeps the bytes verbatim in `editor->prologue`; `editor_panel_save()`
concatenates them back ahead of the serialized body. This is why YAML the parser
only half understood costs the author nothing. **Anything that writes a
document's frontmatter to disk while the editor has that document open must
account for the stale `prologue`, or the next editor save will clobber it.**

Note `wordsmith_frontmatter_set_field()` assumes fenced frontmatter and will open
a new `---` block on a document that has none — wrong for a bare `metadata.yaml`
sidecar, which needs `yaml::set_field` semantics instead.

### Editor buffer model

`GtkTextBuffer` tags mirror `markup.hpp`'s model one-to-one. One block per line,
with two exceptions: a code block's body spans its lines and its fences are not
in the buffer at all; list markers *are* inserted as text but carry `marker_tag`,
so save skips them and regenerates numbering from the list tags. Save walks tags
back through `wordsmith_markup_builder_*`, which keeps escaping and delimiter
placement testable without a display.

Round-tripping is not byte-for-byte lossless (wrapped paragraphs fold into one
line, markers normalise), but it reaches a fixed point after one pass.

### Composition mode

The manuscript alone, full screen: both side panes, the format bar and the menu
bar go away, the window goes full screen, and the editor draws its text as a
centred column.
`win.composition-mode` is a stateful toggle on F11, and Escape leaves as well —
a mode that hides the menu bar has to be escapable without it. Escape goes
through the action rather than calling `ui-state` directly, so the check mark
behind the hidden menu bar comes back right, and the key controller sits in the
bubble phase so anything focused that wants Escape gets it first.

It is a **mode, not a preference**: nothing about it is written down, and it
ends with the sitting.

The centred column is made of the text view's own left and right margins,
recomputed by `editor_composition_margin()` whenever the pane's width changes.
Not CSS — GTK's CSS has no `max-width`; and not a narrower widget around the
view, which would need a viewport and cost `GtkTextView` its line-by-line
layout, on exactly the document that cannot afford to be laid out all at once.
The resize signal is the scroller's hadjustment `notify::page-size`, GTK4 having
no size-allocate signal. `editor_composition_margin()` is the display-free seam
the UI test checks, and it falls back to the ordinary margin whenever the pane
is too narrow for the column — including the width of 0 before the first
allocation.

**What the mode hides is hidden underneath the author's answer, never by
changing it.** The inspector's entry in the session and its check mark both stay
where they were, and everything comes back the way it was on the way in
(`binder_shown_before_composing` and its siblings). Two consequences that are
easy to get wrong, and did not work until they were handled:

- The `ui_state_set_*_visible()` trio records the new answer without moving
  anything while composing. Otherwise restoring a session — Ctrl+O still works
  with the menu bar hidden — puts a pane on screen in the middle of the mode.
- `save_session_now()` writes `pane_answers()`, not what the widgets show.
  Reading the widgets would record "dismissed" for anyone who quits from
  composition mode and lose them the lot for good.

Anything else that comes to be hidden by the mode inherits both rules.

### The format bar

Bold, italic and underline above the manuscript, on by default and remembered
per project like the side panes. **GTK4 has no control for this**: `GtkToolbar`
and `GtkToolButton` went out with GTK3, `GtkActionBar` is a contextual strip for
the bottom of a window, and `GtkTextView` ships nothing of its own. A format bar
in GTK4 is a `GtkBox` with the `.toolbar` style class and ordinary buttons in
it, which is what `format-bar.c` is; `.format-bar` in the stylesheet is the
whole of the look.

The buttons only *name* window actions (`win.format-bold`, …), the way the
binder's context menu names `win.new-text-in`. The bar does not know there is an
editor, and Ctrl+B, the Format menu and the button are three ways into one verb.
It sits in a box above the editor rather than across the window, so hiding the
inspector does not move the buttons.

**The buttons follow the text, never the click.** A press raises the action and
nothing else; what lights a button is `editor_panel_set_styles_callback()`
reporting what is in force where the author stands, through `main-window.c` to
`format_bar_show_styles()`. A press the editor does nothing with — with no
document open — therefore leaves the button where it was rather than lying about
the manuscript. `updating` in `format-bar.c` is what keeps that report from
raising the action again.

`editor_style_flags()` is the display-free seam, and it answers two questions
with one function because the answer has two jobs. Over a selection it reports
only a style that covers the whole of it, which is exactly
`editor_panel_toggle_style()`'s own rule — both go through `tag_covers()`, so a
lit button always means "click to take this off". With no selection it reports
the character *behind* the cursor, the one just typed past, falling back to the
character ahead at the start of a line. `tag_covers()` steps to the tag's next
toggle rather than walking characters, because this runs on every cursor move
and the selection may be the whole manuscript.

The bar has no accelerator, deliberately: the pane chords are worth knowing
because of the rule behind them (the shifted form of the format key sharing the
letter), and there is no format key whose letter this could borrow.

### Typing into a style

Bold with nothing selected is "turn bold on and start writing", so
`editor_panel_toggle_style()` has a second half: it sets `asked_mask` and
`asked_flags` on the panel, and the next text typed comes out wearing them.

Two words rather than one because **off has to be as sayable as on** — Ctrl+B at
the end of a bold word means stop, and a single set of bits could not tell that
from having said nothing. `editor_typed_styles()` and `editor_ask_for_style()`
are the display-free pair the test drives; the panel holds no rule of its own.

The style is asked for **at a place, not for a stretch of time**. Any cursor
move forgets it (`on_cursor_moved`), so Ctrl+B, a change of mind and a click
elsewhere leave nothing waiting. The insertion in progress is the one move that
does not count, since the mark reaches its new home before the text is styled:
`inserting` is that exception, and it is why the cursor handler and the insert
handler are not in a fight over the same answer.

Both ends of `insert-text` are connected. The **before** handler reads what the
spot is wearing while it can still be read, since GTK gives inserted text the
tags *covering* the spot — a different question, and at the end of a bold word
the tag stops short, which is exactly where an author carries on typing in bold.
The **after** handler dresses the landed text, applying the answer and removing
its opposite, so Ctrl+B inside a bold word stops the bold rather than inheriting
it.

One exception keeps paste honest: text that arrives already wearing something
(`range_is_styled()`) is left alone unless the author asked for a style
themselves. A paste of formatted text keeps its own; a paste of bare text takes
the styling of where it lands, which is what every other editor does.

Nothing here touches block tags. Typing at the end of a heading still gives text
without the heading tag, which save reads past — `line_kind()` asks the line's
first character. That is unchanged, and it is the next thing in this corner
worth fixing.

### Undo and redo

Wordsmith owns its history rather than borrowing `GtkTextBuffer`'s, which is
switched off at construction. The built-in stack records text and not tags, so
Ctrl+B was never undoable, and it died at every `set_text()` in
`editor_panel_load()`, so switching documents and coming back found nothing to
take back. `undo-stack.h` carries the full rationale.

**One chronological history per binder item**, holding text, formatting and
metadata edits in the order they were made, and a press addresses **the
selection's** history. Not one stack for the window: Ctrl+Z can then never yank
the view to a document the author had stopped thinking about. A document's
history holds its text, its formatting and its metadata; a folder's holds only
metadata, which is all a folder has. Histories are in memory and cleared by
`ui_state_set_project()` — that is the whole of "session-scoped".

Two things the records carry that they look like they should not, both of which
are quiet data loss if dropped:

- **Text records carry their tag runs.** Undoing an insert is only a delete, but
  redoing one has to put back text that may have been bold, and undoing a delete
  has to restore what the run was wearing. `put_text_back()` clears the inline
  tags over the range before applying them, because GTK gives inserted text the
  tags covering the spot — text put back inside a bold word would otherwise come
  out bold whatever the record says.
- **Style records carry the tag's prior coverage**, not a boolean.
  `editor_panel_toggle_style()` makes a mixed selection uniform, so bold over
  half-bold text is not reversible by pressing bold again; only the record still
  knows the mix.

Offsets are **character** offsets, the units `GtkTextIter` counts in. Anything
using `strlen()` for a record's length works until the first accented character
and then puts text back in the wrong place.

Runs coalesce so one press takes back a word: contiguous, no newline, and
breaking where a word does — at the step from whitespace to a character, so a
space joins the word before it. `undo_records_coalesce()` is the display-free
seam, and `undo_store_break_run()` is what the cursor moving calls, because two
stretches of typing with a click between them are two things done however close
the offsets fall.

**The fingerprint** is the subtle part. A history outlives its buffer, and save
is a lossy round trip through `markup.hpp`, so the text coming back off disk is
not always the text the offsets describe. A history records a digest of the
buffer as it was left and is **dropped rather than replayed** when it does not
match on return — `session_for()`'s contract again. In the ordinary case, markup
already at its fixed point, it matches and the history survives.

`editor->applying` is the guard that keeps an undo from being recorded as a
fresh edit, in the same idiom as `loading`.

`note_style_change()` is a rule the undo work uncovered rather than one it
needs: **GtkTextBuffer only calls itself modified when text moves**, so changing
a tag left nothing saying the document wanted saving. Bolding a word and closing
the project lost the bold, and the title bar never showed the `*`. Everything
that changes a tag on the author's behalf — the toggle and applying a style
record — goes through it. Asking for a style with nothing selected does not: it
changes nothing on disk and is forgotten the moment the cursor leaves.

Metadata records go to `project_actions_apply_metadata_record()` rather than the
editor: the bytes may be a folder's sidecar, and even for a document they are
frontmatter the editor deliberately does not hold. Both directions go through
the same `write_metadata()` a typed edit does, so the ordering against the
editor's stale `prologue` is written down once. A record is keyed by the **item**,
not the file it lands in.

The Edit menu names what a press would take back ("Undo Typing", "Undo Bold",
"Undo Synopsis") and greys out what has nothing behind it. `menu_bar_show_undo()`
reports what is in force rather than deciding it, the way
`format_bar_show_styles()` does; a `GMenuItem`'s label cannot be changed in
place, so the item is replaced.

**File operations are not on the stack yet** — create, rename, move,
drag-reorder, group and delete. Adding one is a new `UndoKind` and an arm in
`undo_record_apply()`, not a redesign. What holds them back is the question text
edits do not raise: what undo should do when the file has changed on disk since
the record was made. Deleting needs it least by design rather than by luck: the
file is in the trash, not gone.

### Naming, and renaming

**Every name is typed into the binder row, never into a dialog.** New Text, New
Folder and New Folder with Selection each create the item untitled and open an
entry over its name; there is no name prompt anywhere in the application.

That means something reaches disk before the author has said what it is called,
rather than a placeholder row standing in the tree until they do. The binder is
a directory scan, so **a row with no file behind it is a row the scan cannot
produce** — a placeholder would be an entry `load_binder()` has no way to return,
which is the one thing the binder's contract does not allow. An author who
dismisses the entry keeps an item called `Untitled`, which is what every file
manager that names this way leaves behind.

`create_untitled_document()`, `create_untitled_folder()` and
`group_into_untitled_folder()` each pick a free name (`Untitled`, `Untitled-2`)
and create the item in the same call. Handing the name back for the caller to
pass to `create_document()` would leave a gap between choosing a name and taking
it, and would put a create that fails on a collision back in the caller's way.
`unused_path()` is the shared helper, and the trash uses it for the same job.

A click on the name of a row **that was already selected** opens the same entry,
the macOS gesture; F2 and the binder's context menu are the other ways in, both
through `project_actions_rename()` so every item command arrives by one door. The click gesture is on the row's label rather than the row, leaving the
icon and the expander's twist arrow alone, and it sits in the bubble phase — so
it runs before the list view moves the selection, and "already selected" means
as of before this click, which is the question being asked.

The name is a `GtkStack` of a label and an entry rather than a
`GtkEditableLabel`, which is this widget already. That type offers no way to
ellipsize the label it shows, so one long chapter name would make the whole
binder scroll sideways, and it starts editing on a double click, which is not
the gesture wanted. Neither is reachable from outside the widget.

A newly created row has no widget yet — the file lands, the binder rebuilds, and
the list view builds rows during its next layout — so `binder_panel_begin_rename()`
leaves the request in `rename_when_bound` and `bind` picks it up when the row
appears. It starts the edit from an idle rather than inside `bind`, since moving
the focus and swapping a stack page in the middle of the list view allocating its
rows is asking for trouble. `binder_panel_select_path()` scrolls to the row it
selects, which is what makes sure the row gets built at all.

`finish_rename()` clears the panel's record of the edit **before** it puts the
label back, because doing so takes the focus off the entry and the focus handler
lands in the same function; clearing first is what makes the second call a
no-op. Enter and clicking away both keep what was typed, Escape and any rebuild
of the binder throw it away — a reload the author did not ask for is not their
answer to the entry, which is why `binder_panel_reload()` cancels explicitly
rather than leaving it to each row unbinding.

`Project::rename_entry()` sanitises the typed name the way a created item's is,
so what the author types is not always what the file is called; the binder shows
filenames, and the `title` in a document's frontmatter is a separate thing left
alone. Where the parent records an order the item **keeps its place** in it,
through `rename_child()` — forgetting the old name and remembering the new one
is a move, and fixing a typo in chapter three should not send it to the end.

### Deleting

**Deleting is a move, never an `unlink`.** `Project::trash_entry()` puts the item
under `.wordsmith/trash/`, beside the snapshots and inside the same hidden
folder, so what Wordsmith keeps on the author's behalf costs disk in one place
and is swept in one place. The trash **mirrors the project's layout**, for the
reason snapshots do: the path itself records where the thing came from, and it
can be found with a file manager and no help from us. A name already taken gains
a `-2` before its extension, so deleting two false starts that shared a name
keeps both.

Nothing sweeps the trash. A cap here would mean silently destroying something the
author was told was recoverable, which is worse than the disk it costs.

It still asks first. Nothing is destroyed, but the trash is a folder they have to
go and find, and a chapter that leaves the binder without a word is
indistinguishable from one that was lost. Cancel is both the default and what
dismissing the dialog means, so the answer that costs nothing is the one a reflex
gives.

`win.trash` has **no accelerator**, deliberately. Delete is the key it would
want, and an application accelerator outranks `GtkTextView`'s own bindings — the
same rule that makes the Edit menu drive the buffer itself for cut, copy and
paste. Binding it would take the Delete key away from the manuscript to give a
menu item a shortcut.

`project_actions_trash()` commits the editor first, the way every move does, so
the words just typed go into the trashed copy rather than being lost on the way
there. Trashing a folder takes what is inside it, so the editor and inspector are
closed when what they were showing is the item **or anything under it**, and the
item's undo history goes with it.

### UI wiring

`WordsmithUiState` is shared per-window state; panels borrow it and never own it.
Panels do not touch the project themselves — they fire callbacks
(`BinderSelectFn`, `BinderMoveFn`, `EditorModifiedFn`, `EditorStylesFn`),
`main-window.c` picks the verb, and `project-actions.c` holds the verb and any
dialog flow. The binder's
context menu only *names* window actions (`win.new-text-in`, etc.), which
`menu-bar.c` installs — that indirection is what keeps the panel from needing to
know about the window.

Any operation that moves a document saves first: the move takes the editor's path
out from under it. `save_and_remember_open()` / `settle_after_move()` are the
pattern.

The stylesheet is `src/ui/style.css`, bundled as a GResource (built with
`--manual-register`, since `wordsmith-ui` is a static library and a
constructor-only object would be dropped from the link).

## Conventions

Headers carry the design rationale — the *why*, the alternatives considered, and
the named seams. Read the header before the implementation, and when a decision
changes, update the header comment with it. Implementation files comment
sparingly, only where the code alone would mislead.

Commit directly to `main`, one commit per feature.
