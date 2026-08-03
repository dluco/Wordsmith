# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

Dependencies: GTK4 ≥ 4.12 (via pkg-config), Enchant 2, `glib-compile-resources`,
CMake ≥ 3.31. The `argo` JSON library is a git submodule — `git submodule
update --init` before a first build.

Enchant is a build dependency (`libenchant-2-dev` on Debian); the *dictionaries*
it reads are not. A machine with none installed builds, runs and passes its
tests — nothing is marked, and the spelling tests that need one skip themselves.

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

Whether spelling is checked is the second, and the first boolean one. It takes
the session flags' rule rather than the text size's clamp: **only an outright
`false` turns it off**, and every other way of not saying so — no key, the wrong
type, an unparseable file — leaves it on. Anything boolean added here inherits
that, and the default to pick is the one that costs nothing when it is wrong.

Whether a typed list marker makes a list (`autoformat-lists`) is the third, and
the first to *inherit* that rule rather than state it. `read_flag()` is where it
is now written down, and both booleans go through it; `set_flag()` in
`preferences-c.cpp` is the same for the write, which is read-modify-write
because saving re-emits every field. A third flag is two more lines in
`load_preferences`/`save_preferences` and a pointer-to-member at the bridge.

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

**A list item's marker belongs to the serializer, and to nothing else.** The
panel says *that* an item is ordered and what number it carries, through
`wordsmith_markup_builder_set_list()`, and `markup.cpp` writes the `- ` or the
`1. `. Writing one into a span as well is not a duplicate that cancels out — it
is a marker in front of a marker, so `- an item` saves as `- - an item` and
gains another every time the document is opened and saved. That is precisely
what happened while the builder had no way to state ordering and the panel
encoded it in a span instead; the fix was to give the builder the answer rather
than to teach the panel to draw. `set_list()` is the same shape as `set_code()`
and for the same reason: a fact only one kind of block has, stated rather than
written into the text.

A block tag covers its line's **newline** as well as its text. That is what lets
an *empty* line carry one — the newline is the only character there — which is
what "make this line a heading and start typing" needs between the pick and the
first keystroke. `line_kind()` asks the line's first character either way.

Save **drops a block with nothing in it**, whatever kind it is (`line_is_empty`).
For a paragraph that is the author's spacing, which the serializer puts back
between blocks anyway. For the rest it is the other end of the rule above: a
style asked for on a line nothing has been typed into yet would otherwise commit
a bare `#` and a trailing space to the manuscript. Nothing is lost, because there
were no words there to lose. A list item's **marker does not count as content** —
it is display that save regenerates, so an item with no words is as empty as a
blank line.

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

A block style dropdown, then bold, italic and underline, then the two list
toggles, above the manuscript — on by default and remembered per project like
the side panes. **GTK4 has no control for this**: `GtkToolbar`
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

The dropdown and the two list buttons follow the text by the same rule, through
`editor_panel_set_block_callback()` and `format_bar_show_block()` — one call
moving all three, off the same event: `notify_format()` fires both halves at
once so nothing on the bar can drift a keystroke apart. `EDITOR_BLOCK_OTHER`
leaves the dropdown showing nothing and both list buttons out, rather than
picking an answer it could not give — a selection spanning two kinds, a code
block, no document open. `updating` covers them too, a `GtkDropDown` reporting
every change to its selection the way a toggle does.

The list buttons name `win.block-style` with a target rather than an action of
their own, so the button, the Format menu and Ctrl+Shift+8 stay three ways into
one verb — **including the way back out**, which is the verb's own doing and not
something the bar knows about. That is what keeps a press on a lit button and a
press of its chord from meaning two different things.

The bar itself has no accelerator, deliberately: the pane chords are worth
knowing because of the rule behind them (the shifted form of the format key
sharing the letter), and there is no format key whose letter this could borrow.
The styles *in* it have chords of their own; see Block styling.

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

The after handler dresses the **block** as well, and for the same reason the
inline half exists: at the end of a heading the heading tag stops short, so
carrying on typing a title used to give plain characters that save read past.
`dress_block()` applies the line's own kind over what landed — but only as far
as the **first newline in it**. The line a return opens gets nothing from this,
because the block ends where the text does; a paste of several paragraphs into a
heading takes the heading on its first line and leaves the rest alone. What a
return carries across is decided separately, below.

There is no `range_is_styled()` exception for blocks. A line is one block, so
text pasted into the middle of a heading is part of that heading whatever it
arrived wearing — the inline exception exists because two styles can share a
character, and two blocks cannot share a line.

### Block styling

What a line *is*, as against what its characters are wearing: Paragraph,
Heading 1 to 3, Block Quote, Bulleted List, Numbered List. **One answer per
line, not a set of flags** — a line is a heading *instead of* a paragraph where
it can be bold *as well as* italic — and that is why it is a dropdown in the
format bar rather than six more toggles, which could only ever have one lit.

Five of the seven live in the dropdown. **The two list styles also get toggle
buttons**, after the inline ones and behind a separator of their own. A list is
the block kind an author turns on and off rather than picks — a paragraph
becomes a list and stops being one, where it does not stop being a heading but
becomes something else instead — and on-and-off is what a toggle draws. Both
controls show the same line: standing in a bulleted list lights the button *and*
reads "Bulleted List" in the dropdown.

That split is what decides the **"off" rule, and it is not the same for all
seven**. Asking for a list where one already covers the whole of what is
addressed turns those lines back into paragraphs, exactly as pressing Bold over
bold text does, because a lit button has to mean "click to take this off". The
other five have no button, are reached from a dropdown where "the same row
again" is not a gesture that could mean off, and stay a straight pick: asking
for the kind a line already has does nothing, and Paragraph is the way back.
`editor_block_style_for_press()` is the whole of that rule and the display-free
seam for it — the same shape `editor_ask_for_style()` is, a press folded against
what is in force. `block_style_over()` supplies the "in force", and only a kind
covering *every* line addressed counts, so a selection half in a list becomes a
list rather than half of one going away.

An undo record is named for **what the author asked for, not what the lines
became**: taking a list off reads "Undo Bulleted List", the way a style record is
called "Bold" whichever direction the press went.

### Enter inside a list

**Lists are the only block kind a return carries across.** Enter in a list item
opens another one; Enter in an item holding nothing but its marker ends the
list, turning that item into a paragraph. Enter anywhere else is unchanged — a
heading is one line, and the thing after it is not another heading.

`editor_return_action()` is the display-free seam and the whole of the rule,
decided in the **before** handler because both facts it needs stop being true
once the newline lands: the item is no longer the cursor's line, and no longer
empty in the way that matters.

Only a **lone newline** counts. A paste that happens to contain one is not a
return being pressed, and turning every line of a dropped page into an item is
not what it asks for.

**Anything that edits the buffer from inside the `insert-text` handler owes the
handlers behind it a call to `revalidate()`.** GTK hands every after-handler the
same `location` iterator and an iterator does not survive a mutation, so the
marker this inserts leaves whoever is connected later reading a dead one —
`spell-check.c` is exactly that, created after the panel's handlers on purpose,
and it reads `location` for the line to recheck and for where the author is
typing. Without it GTK warns on every Enter inside a list and the marking reads
a garbage offset. The insert mark is the right answer rather than an arithmetic
correction, because what the iterator means to anyone behind us is where the
text that just arrived has left the author.

Ending a list is the answer that has to **refuse the newline**, which is why the
decision is made before rather than corrected after:
`g_signal_stop_emission_by_name()` in a handler connected the ordinary way keeps
it from ever reaching the buffer, `insert-text` being `G_SIGNAL_RUN_LAST` with
the insertion in its class closure. The item then becomes a paragraph through
the ordinary verb, so it is one block record.

Continuing one is two edits — a newline and the item below it — and **that is
what compound records are for**. The item's record says the new line was a
*paragraph*, which is not quite what it was, because it did not exist: that is
the truthful thing to say about a line the other half is about to delete, and it
is what makes undo strip the marker before the newline goes rather than leaving
`- ` behind as ordinary text. The style is applied whatever the new line already
reads as, because GTK gives the inserted newline the tags covering the spot, so
the line below already answers as an item — with no marker in front of it, which
is the state this exists to fix.

The buffer carries more than the UI offers: headings 4 to 6 and code blocks both
load, save and round-trip. A line wearing one reads back as
`EDITOR_BLOCK_OTHER`, which the dropdown shows as *no* answer rather than a
wrong one, and **a pick passes over it rather than taking it**. That is undo's
requirement rather than taste: a record says what each line was as an
`EditorBlockStyle`, so a line that was a code block has no way to say so, and
taking it over would be a change no press could put back. Making one of these
reachable is the same work in both directions — a style to offer, and a style a
record can name.

`editor_block_style_at()`, `editor_block_apply()` and `editor_block_renumber()`
are the display-free seams, driven through an `EditorBlockTags` — the same shape
`editor_style_flags()`'s `inline_tags` is, and for the same reason. Applying
never adds or removes a line; the most it moves is the list marker at the head
of one, which is what lets an undo record address lines **by number**.

Numbering is regenerated on save either way, so `editor_block_renumber()` exists
for what the author reads while they work. It **reaches out to each run's real
ends** rather than renumbering what it was handed, or joining two lists by
naming the line in the gap would count 1, 2, 1, 1, 2.

The seven are one action, `win.block-style`, taking the style's id as a string
parameter — not seven actions, because the dropdown has to be able to *name* the
one it raises. `editor_block_style_name()` / `editor_block_style_id()` is the
single table, and four places read it: the dropdown, the Format menu, the
accelerators, and the word Edit ▸ Undo uses.

The chords are **Ctrl+Alt+0 to 3** for Paragraph and the three headings, and
Ctrl+Shift+7/8/9 for Numbered, Bulleted and Quote. Ctrl+0 is the text size's, so
the heading family moved to Ctrl+Alt *whole* rather than letting Paragraph alone
take an exception — a set of chords is worth knowing because of the rule behind
it, and "Ctrl+Alt and the heading's number, except the one meaning no heading"
is not a rule. The other three have no number of their own to borrow and take
Google Docs' assignment. Each carries the shifted symbol as an alternate for the
reason the text size does: on a US or UK layout Ctrl+Shift+7 reaches the keymap
as `ampersand`.

### Deleting at a marker

The other way out of a list, and the one an author reaches for first: **a
deleting key that runs into a marker is spent taking that marker off.** The item
becomes a paragraph, nothing is deleted, and the words stay. Joining an item to
its neighbour is then two presses — one to stop being an item, one to merge —
which is what Word and Google Docs do.

It has to be a rule rather than something left to the buffer because a marker is
**tagged non-editable**, which makes a deletion near one wrong in one of two
ways:

- A range that is nothing but marker is **refused outright** by
  `gtk_text_buffer_delete_interactive()`, so the key does nothing whatsoever.
  Deleting an item's last word and pressing Backspace again left the caret stuck
  behind a `- ` no key would remove; Delete at the head of an item is the same
  press from the other side.
- A range that **crosses the line boundary** is not refused, and is worse: the
  newline goes, the marker is skipped, and it lands mid-line, where nothing
  draws it as a marker and save drops it. `- one` + Delete + `- two` showed
  `- one- two` and saved `- onetwo` — the screen and the manuscript come apart.

Two display-free seams are the whole of it, in the shape `editor_return_action()`
is. `editor_delete_leaves_list()` answers for the caret's **own** line, and the
two directions reach differently at the ends, each stopping where its own
justification runs out: backward from column 0 drags the marker onto the line
above, backward from the marker's far side is the refusal; forward from column 0
is the refusal, and forward from the far side is the author's own text.
`editor_delete_leaves_next_list()` answers for the line **below**, which the
caret's own line cannot see — a forward press at the end of a line eats the
newline first, whatever else it goes on to eat.

Both are asked of **every** deleting key: `GtkTextView::backspace`, and
`delete-from-cursor` for Delete, Ctrl+Delete, Ctrl+Backspace and the two
line-clearing chords. Which key it was does not change what a marker in the way
is worth, and a rule that held for one of them would be one an author could only
find by accident. Those two signals are the only place a press against
non-editable text can still be seen, and both handlers stop the emission.

The line goes through `set_block_style_over()` — the body of
`editor_panel_set_block_style()`, split out because these presses address a line
the cursor is not standing on. So leaving a list this way, picking Paragraph
from the dropdown and pressing a lit list button are one path and one record: it
reads "Undo Paragraph" and costs one press to take back.

`take_back_autoformat()` is asked **first** on Backspace, because where a marker
has just been made out of typed characters those characters are what the press
asks to have back, and taking the list off instead would take them with it. It
is deliberately not offered a second door on `delete-from-cursor`: a chord that
deletes a word or a line is not an author saying "I meant a hyphen".

What this does not reach is a deletion the buffer performs itself rather than a
key binding raising — a selection spanning two items, a cut, a paste over one, a
drag. Those get the answer below instead, which is a different one.

### One marker per line, and the buffer draws it

**A marker is derived, and is never in the undo history.** The block tag is the
only record that a line is an item; the `- ` is drawn from it, saved by
regenerating it from it, and redrawn from it whenever an edit moves a line
boundary. A record of the marker would be a second answer to a question the
block tag already answers, free to disagree with it.

That is what makes the repair below possible at all. A deletion that **joins two
lines** strands the lower line's marker in the middle of the joined one — the
marker is non-editable, so `gtk_text_buffer_delete_interactive()` steps over it
and deletes around it. It is then drawn but belongs to nothing, and save skips
it, so `- one` + `- two` showed `- on- wo` and saved `- onwo`. Every door the
key bindings above do not cover arrives here: cut, paste over a selection, drag,
and a selection deletion the keys hand to the buffer whole.

`redraw_markers()` is the repair, and `strip_markers()` is what makes "one
marker, at the head" true by construction rather than assumed — it takes every
marker off the line, so a well-formed line comes out unchanged and a joined one
comes out with neither, and then the line's own kind draws the one it wants.
`editor_block_apply()` goes through it too, so a pick cleans a line up the same
way.

Two things it deliberately does not do:

- It touches **markers and nothing else**, and is not `editor_block_apply()` over
  the line. A join leaves the second line's own block tag on its half, invisible
  to every reader here — `line_kind()` and `editor_block_style_at()` both ask the
  line's *first* character — and that stale-looking tag is precisely what puts
  the line back as what it was when the join is undone. Clearing the line the way
  a pick does loses it, and undoing a paragraph that swallowed an item then hands
  back a paragraph. It did exactly that until the end-to-end run caught it.
- It makes **no undo record**, running under `applying`. It does not need one:
  undoing the deletion restores the text, and the marker is derived again from
  what the lines then are. `apply_single_record()` is the other call site, for a
  text record carrying a **newline** — the only kind that can move a boundary, in
  either direction. Plain typing put back inside an item cannot.

The strays sit exactly at the join, which is the offset the deletion's own record
already holds, so stripping them moves nothing that record is pointing at. The
one place a redraw does change a width is the tenth item of a numbered run.

### Typing a list into being

`- ` at the head of a paragraph makes it a bulleted item and `1. ` a numbered
one: the marker the author typed comes out of the text, and the one the list
draws for itself goes in. It is the gesture every editor with lists has, and the
one an author reaches for long before they find the button.

What makes it safe to do without being asked is that **saying no is one
keystroke, and either keystroke an author would try**. The conversion is its own
undo record, pushed after the space's own text record rather than folded into
it, so Ctrl+Z leaves a paragraph reading `- ` and whoever meant a literal hyphen
carries on typing. A second press takes the characters as ordinary typing. One
record for both would leave them nothing to do but type the same three
characters again.

**Backspace is the other way out, and it has to be caught at the key binding.**
A list marker is tagged non-editable, so a backspace against one never arrives
as a deletion at all — before `take_back_autoformat()` it did nothing
whatsoever, which is a list that made itself and then would not go away for
anyone who had not thought of Ctrl+Z. `GtkTextView::backspace` is the only place
the press can still be seen, and stopping that emission is what keeps the view's
own binding off it. It goes through the same undo record Ctrl+Z does, so the two
cannot come to mean different things; `autoformat_at` is what says the record on
top is still the conversion, and **anything at all happening forgets it** — a
keystroke, a deletion, the caret moving. The one difference is where the caret
is left: undo lands it on the line it changed, as any block undo does, while a
backspace leaves it after the `- ` that just came back, or the next press would
join the line to the one above instead of eating the marker. Which is also why
this is the question `on_backspace` asks first — see Deleting at a marker above,
whose answer would take those characters away with the list.

`editor_autoformat_style()` is the display-free seam and the whole of the rule,
the same shape `editor_return_action()` is. It is deliberately narrow, because
every case it gets wrong is a character taken away from the author: a **lone
space** (a paste carrying one is not a key being pressed), on a **paragraph**
(a hyphen heading a heading, a quote, or an item already, is a hyphen), and the
line reading **exactly** `- ` or `1. ` — which is also what says the marker is at
the start and the cursor at the end, so neither is asked separately. `1.` and no
other number, since the number is not kept and the list counts itself from 1.
Not `* `, `+ ` or `1) `. Whether the panel is listening is the preference, asked
separately: what a keystroke means and whether it is acted on are two questions.

**The list goes on first and the typed characters come out from behind it**,
which is the compound's order as much as the buffer's. A compound is named for
its first part, so the other way round Edit ▸ Undo would read "Undo Typing",
naming the bookkeeping rather than the gesture. It also happens to be the order
that is right in both directions. The typed characters are then found as
*whatever follows the drawn marker* rather than by counting: the two are the same
width today and have no reason to stay that way.

`editor_autoformat_lists()` holds the answer, read from the config file the
first time anything asks, for the reason `spell_check_wanted()` holds its own —
the check mark and what a keystroke does must not be free to disagree. The menu
item is in **Format**, not beside the spelling in Edit: what it turns on and off
is a block style being picked, by typing instead of from a menu, so it belongs
with the styles it applies.

### Spelling

A red line under the words the dictionary does not have. Split across the
layers the way everything else is, and the split is the load-bearing part:
`core/spelling.hpp` holds **what counts as a word** and **whether it is
spelled right**, `ui/spell-check.c` holds **where the line goes**.

The core half is there rather than in the UI because a misspelling is a fact
about the manuscript, not about how it is drawn. Enchant is a plain C API over
whatever dictionaries the system has and its header pulls in nothing but
`<stdint.h>`, so it does not cost `src/core/` the "no GLib" rule. `words_in()`
has no dictionary behind it at all, which is what lets the rules that are worth
arguing about — an apostrophe holds `don't` together, a hyphen breaks
`well-known`, an em dash is not a hyphen, anything with a digit in it is not
offered — be tested on a machine with no dictionary installed.

It does take the dictionary's *answer* to the same question:
`SpellChecker::extra_word_chars()` is enchant's list of non-letters allowed
inside a word (`0123456789’` for English), and every character in it holds a
word together from the inside exactly as an apostrophe does. A **string**, not a
dictionary, so the contract above is intact. The point is a language held
together by something this file never thought of, not English, where the list
says nothing the rules do not already cover — and where several backends answer
with nothing at all.

**The rules win where they have an opinion**: the list can only turn a break
into part of a word, never the reverse. Enchant's header warns the answer may be
a guess, and the hyphen is refused outright however loudly it is named, because
joining `well-known` asks about a compound no dictionary holds and puts a red
line under an ordinary word. (`enchant_dict_is_word_character()` is the API that
would have done exactly that: on this machine the `en_GB` dictionary answers
*yes* for the hyphen.)

**Every offset is a character offset**, the unit `GtkTextIter` counts in. Same
rule as the undo records, same reason: bytes work until the first accented
character.

**Nothing here ever fails.** No dictionary installed means the checker knows
every word, so the manuscript comes up unmarked rather than solid red, and
nothing reports an error. That is the same trade `preferences.cpp` makes.

Three things are deliberately not marked, and each is a false mark avoided:

- **The word being typed.** Without it every word flashes red while it is being
  written. `spell_check_word_being_typed()` is the display-free seam, and it
  asks two questions rather than one. The cursor has to be *in* the word, and
  the text has to have been last edited *where the cursor now is* — otherwise a
  click into a misspelling takes its own mark off, which answers the question
  the author clicked to ask by hiding it. Within the first, the asymmetry is the
  subtle part: the **trailing** edge counts because the cursor sits at the end
  of a word for as long as it takes to type one, and the **leading** edge does
  not because deleting the word in front of the cursor leaves it there having
  written nothing.
- **Anything wearing a tag given to `spell_check_skip_tag()`** — the editor
  hands it the two code tags.
- **Everything**, when the preference is off.

Rechecking is a **line at a time**, and a line here is a whole block. Anything
finer would have to work out for itself that deleting a space makes one word
out of two. `spell_check_hold()`/`release()` is what keeps a document load —
several hundred insertions — from checking the same lines once per block.

The marker follows the buffer's own signals rather than being driven by the
editor panel, and is created **after** the panel's handlers so it reads the
tags the panel has finished applying. It must be freed before its buffer:
`spell_check_free()` disconnects, or the next edit reaches into freed memory.

One consequence reached beyond spelling. `add_spans_for_range()` walks to the
next toggle of *any* tag, so a decoration splits a run as readily as bold does;
it now gathers text and hands it over only when the flags change. Two spans with
the same flags would each get their own delimiters, and a bold word with a
squiggle under half of it would save as `**wo****rd**` — the same words, and a
diff in the author's file every time they opened it. **Any future tag that is
decoration rather than markup inherits this**: it is safe because the span
walk coalesces, not because the tag was careful.

A right click on a marked word offers what it might have been, and the two ways
of saying it is a word: **Ignore All** for this sitting (`accept()`) and **Add
to Dictionary** for good (`remember()`). **They come first in the menu**, and
that requirement is what decides the design. GTK4 has nothing like GTK3's
`populate-popup`, and its one hook — `gtk_text_view_set_extra_menu()` — joins a
model onto the **end** of the view's own, with nothing to choose where. A menu
whose first item answers what the author clicked cannot be built that way.

So the press is **taken, not decorated**. A secondary-click gesture in the
**capture** phase runs ahead of `GtkTextView`'s own (GTK adds that one in the
bubble phase); on a marked word it claims the sequence, so the view never opens
its menu, and puts up a popover of ours instead. Every other press is left
alone, so the ordinary context menu is untouched. The model is refilled per
press and the popover built once from it, since GTK follows a `GMenu`'s
`items-changed`.

The price is that this file now carries the editing verbs too — Cut, Copy,
Paste, Delete, Select All, Insert Emoji — as `GtkTextView`'s own actions under
its labels and in its order, so the two menus are one menu with a spelling
section on top. Two exceptions:

- **Undo and Redo name `win.undo`/`win.redo`**, not the view's `text.undo`,
  which drives the buffer history that is switched off at construction.
- **Cut, Copy and Paste are enabled by hand** in `update_editing_items()`. GTK
  does that in a static function on its way up, so a menu naming those verbs
  must answer the same questions or offer a Paste that does nothing. This is the
  part most likely to drift as GTK changes.

A menu raised from the keyboard (Menu, Shift+F10) is the view's own and holds no
spelling: nothing can prepare it, and the word at the cursor is the word being
typed, which is never marked. `spell_check_fill_menu()` is the display-free
seam; the gesture that shows it is not testable headless.

The spelling items name actions in a `spelling` group the marker installs **on
the view**, not window actions like the binder's context menu — those verbs move
files and open dialogs, these three change one word in one buffer. A correction
reaches the buffer as a plain delete and insert, so the editor's own handlers
record it, mark the document modified and recheck the line — **two undo records
rather than one**. `UNDO_COMPOUND` now exists and is what fixes this; the
correction is simply not wired through it yet, and doing so is a matter of
gathering the two records the handlers already make rather than any new
machinery.

`SpellChecker::accept()` is the seam the manuscript's own vocabulary will arrive
through — a novel is full of names that are spelled right and are in no
dictionary. It adds to enchant's session list, so nothing is written to disk;
`remember()` is the permanent form, for an author saying "this is a word".
Nothing reads the manuscript for names yet.

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
- **Block records carry every touched line's prior kind**, for that same reason
  and one more: **one pick is one press to take back**. Making a chapter's twelve
  paragraphs into a list costs one Ctrl+Z and not twelve, because the count an
  author presses has to match the count of things they did. Only the lines that
  actually changed are in the record, which is what lets undoing a pick over a
  mixed selection put every line back to its own kind rather than to one of them.
  The list markers a pick inserts and deletes belong to the block record too:
  `apply_block_lines()` sets `applying` so they never arrive as text edits of
  their own, and regenerates them by re-applying the style in whichever direction
  is being asked for. That one function is the path a pick and an undo of a pick
  both take, so neither can grow a rule the other lacks.

- **A compound record is several records that were one thing done.** One
  keystroke has to cost one press to take back, and Enter inside a list is a text
  edit *and* a block edit. Parts are applied **in order forwards and in reverse
  order backwards**, which is the whole of what makes a compound different from
  the records inside it — taking back that Enter has to lift the item off before
  the newline goes, or the marker is left behind as text in the manuscript.
  `undo_record_new_compound()` drops NULL parts and hands back a lone survivor
  unwrapped, so a caller that may or may not have a second thing does not branch.
  Compounds hold **buffer records only**: one holding `UNDO_METADATA` would have
  to be split across `project-actions.c` and the editor mid-sequence.

`UNDO_BLOCK` is **ignored by `undo_record_apply()`** and applied by
`editor_panel_apply_record()` instead, the way `UNDO_METADATA` is applied by
`project-actions.c`: the block tags and the markers made out of them are
`editor-panel.c`'s half of the buffer. It addresses lines **by number**, which is
sound because setting a block style never adds or removes a line. Its `verb` is
carried on the record rather than looked up, because the table of style names
belongs to the editor panel and the store has no business knowing what a heading
is. `UNDO_COMPOUND` is ignored there for a third reason: it may hold either
kind, so walking it in both places would split one sequence across two passes
and get the order wrong. `editor_panel_apply_record()` is the one place that can
apply every kind, so it is the one place that unpacks a compound. A compound is
named for its **first** part, that being the gesture the author made — Enter
inside a list reads "Undo Typing".

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
"Undo Heading 1", "Undo Synopsis") and greys out what has nothing behind it. `menu_bar_show_undo()`
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

The name is a `WordsmithEditableLabel` (`editable-label.c`), not a
`GtkEditableLabel`, which is this widget already. That type offers no way to
ellipsize the label it shows, so one long chapter name would make the whole
binder scroll sideways, and it starts editing on a double click, which is not
the gesture wanted. Neither is reachable from outside the widget.

Ours is a `GtkStack` of a `GtkLabel` and a **`GtkText`**, and the `GtkText` is
the whole of why the name stays put when an edit opens over it: a `GtkEntry` is
a frame, padding, a minimum height and a focus ring around a `GtkText`, so
swapping one in shifts the name sideways and draws a box the size of the row
around it. The stack is homogeneous, so the row does not change height either.

The widget has a **CSS name of its own** and deliberately not
`GtkEditableLabel`'s, which its nodes would otherwise match. A theme paints that
widget like a field — the view background, the view's foreground colour — which
in a sidebar row is a darker slab cut out of the list, and on a selected row
fights the selection it sits inside. Under its own name the `text` node matches
nothing a theme knows, and inherits the row's colour like any other label, so
`.binder-name` in the stylesheet is the whole of the look: the same padding on
both faces, and a selection washed in the text's own colour rather than the
accent, which would be invisible on the row renaming always starts from.

The widget owns the three ways *out* of an edit and reports each through
`::editing-done`; the panel owns the gesture *in*, and what an accepted edit
means. Committing an empty name is the widget's business to allow and the
panel's to refuse — a widget showing names has no business deciding that an
empty one means something, so `on_name_editing_done()` puts the old name back.

A press on the row being renamed, anywhere but the entry, is **swallowed**
(`on_row_press_while_renaming`, a capture-phase gesture on the list view). A list
row grabs the focus on every press it sees, and taking the focus off the entry is
what ends an edit — so without this, clicking the icon of the very item being
named commits the name and lights the row's keyboard focus ring. Presses inside
the entry are let through (GtkText claims them first, to place the cursor), and
so are presses on any other row, which end the edit the way clicking away always
has.

A newly created row has no widget yet — the file lands, the binder rebuilds, and
the list view builds rows during its next layout — so `binder_panel_begin_rename()`
leaves the request in `rename_when_bound` and `bind` picks it up when the row
appears. It starts the edit from an idle rather than inside `bind`, since moving
the focus and swapping a stack page in the middle of the list view allocating its
rows is asking for trouble. `binder_panel_select_path()` scrolls to the row it
selects, which is what makes sure the row gets built at all.

`wordsmith_editable_label_stop_editing()` drops the widget's own record of the
edit **before** it puts the label back, because doing so takes the focus off the
entry and the focus handler lands in the same function; clearing first is what
makes the second call a no-op. Enter and clicking away both keep what was
typed, Escape and any rebuild of the binder throw it away — a reload the author did not ask for is not their
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
