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

### UI wiring

`WordsmithUiState` is shared per-window state; panels borrow it and never own it.
Panels do not touch the project themselves — they fire callbacks
(`BinderSelectFn`, `BinderMoveFn`, `EditorModifiedFn`), `main-window.c` picks the
verb, and `project-actions.c` holds the verb and any dialog flow. The binder's
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
