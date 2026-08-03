---
name: run-wordsmith
description: Build, launch and drive the Wordsmith GTK4 desktop app headlessly. Use when asked to run, start, build, test, or smoke-test Wordsmith, or to confirm a UI change works in the real running app rather than only in the headless unit tests.
---

# Running Wordsmith

Wordsmith is a GTK4 desktop app in C/C++. There is no display in CI and **this
project does not do screenshot testing** — a UI change is confirmed through
display-free seams and, when you need the real thing, by driving the running
app and reading its answers as text.

`driver.py` is how you do that. It launches the actual binary against a headless
`mutter` compositor and reaches in two ways, both of which answer in text:

- **the window's GAction group over D-Bus** — every menu item, chord and
  format-bar button is one of these (`src/ui/menu-bar.c` installs them). The
  group takes activations *and* reports each action's enabled flag and current
  state, so "press Ctrl+B" and "is Undo greyed out" are both one call.
- **the AT-SPI tree** — the widget hierarchy with names, plus the editor's
  `GtkTextView` behind AT-SPI's `EditableText`, which is what lets text go into
  the buffer and come back out.

All paths below are relative to the repo root.

## Prerequisites

Everything needed is already installed here. To confirm on a fresh machine:

```sh
pkg-config --modversion gtk4 enchant-2
python3 -c "import gi; gi.require_version('Atspi','2.0'); from gi.repository import Atspi; print('Atspi ok')"
command -v mutter dbus-run-session cmake
```

Build needs GTK4 ≥ 4.12, Enchant 2, `glib-compile-resources` and CMake ≥ 3.31.
The driver additionally needs `mutter` (headless compositor), `dbus-daemon`
(for `dbus-run-session`), `at-spi2-core`, `gir1.2-atspi-2.0` and `python3-gi`.
Dictionaries are *not* needed — with none installed the app runs and the
spelling tests skip themselves.

## Build

```sh
git submodule update --init
cmake --preset debug
cmake --build --preset debug -j
```

## Run: the driver (agent path)

Make a throwaway project, then feed the driver commands on stdin. It must run
under `dbus-run-session` — both the a11y bus and the app's own bus name need a
session bus of their own — and under its own `XDG_*` dirs so a run cannot
inherit or clobber your real preferences and session.

```sh
S=$(mktemp -d)
.claude/skills/run-wordsmith/new-project.sh $S/proj
XDG_CONFIG_HOME=$S/config XDG_STATE_HOME=$S/state \
  dbus-run-session -- python3 .claude/skills/run-wordsmith/driver.py $S/proj <<'EOF'
pick chapter-one
text
sel 18 24
do format-bold
buttons
do save
wait 1
sh cat Manuscript/chapter-one.md
EOF
```

That run prints the buffer, shows `Bold (Ctrl+B)  ON`, and the `sh` at the end
shows `It was a dark and **stormy** night.` on disk under its untouched
frontmatter.

The canned regression script covers the app's signature behaviours — frontmatter
staying out of the buffer, a style surviving the save round trip, undo, typing
`- ` into a list, Enter carrying it, compound undo, one marker per line across a
join and back, and composition mode:

```sh
S=$(mktemp -d)
.claude/skills/run-wordsmith/new-project.sh $S/proj
XDG_CONFIG_HOME=$S/config XDG_STATE_HOME=$S/state \
  dbus-run-session -- python3 .claude/skills/run-wordsmith/driver.py \
  $S/proj .claude/skills/run-wordsmith/smoke.txt
```

It exits non-zero if any `expect` fails, and prints the file it read.

### Commands

| | |
|---|---|
| `tree [depth]` | accessible tree: role, name, interfaces |
| `rows` | binder rows in order (a row being renamed has an empty name) |
| `pick <name>` | select a binder row, which opens it in editor and inspector |
| `text` | the editor buffer with its character count |
| `type <text>` | insert **one character at a time**, as a keyboard would |
| `insert <text>` | one insertion — the paste path |
| `caret <n\|end>`, `sel <a> <b>`, `del <a> <b>` | caret and selection, in character offsets |
| `do <action> [param]` | activate a window action, e.g. `do block-style heading-2` |
| `state <action>` | one action's enabled flag and state |
| `actions [filter]` | every window action, with state |
| `buttons` | format bar: what the dropdown reads, which toggles are lit |
| `press <widget-name>` | invoke a widget by accessible name |
| `expect <file> <substring>` | assert against a file in the project; failing sets exit 1 |
| `shows <substring>` | assert against the editor buffer; the same, for the screen |
| `sh <cmd>` | shell, cwd is the project directory |
| `logs` | the app's own stdout/stderr |
| `wait [sec]`, `help`, `quit` | |

`\n`, `\t` and `\s` (a literal space) are the escapes in `type` and `insert`.

`do <action>` takes any name `actions` lists — `save`, `undo`, `redo`,
`new-text`, `new-folder`, `format-bold`, `composition-mode`, `show-binder`,
`spell-check`, `autoformat-lists`, and `block-style` with one of `paragraph`,
`heading-1`, `heading-2`, `heading-3`, `quote`, `bulleted-list`,
`numbered-list`.

## Run: a human at a desk

```sh
./cmake-build-debug/src/driver/wordsmith [project-dir]
```

Opens a window on your own display. No use headless, and no use to an agent —
there is no way to read it back.

## Test

```sh
ctest --preset debug          # 17 suites, ~0.4s
ctest --preset debug -R yaml  # one suite by name
```

All 17 pass. These are the headless unit tests over the display-free seams, and
they are the first thing to run — the driver is for what they cannot reach.
`--preset debug-asan` is the same build under AddressSanitizer.

## Gotchas

- **Broadway cannot work, though it looks like the obvious headless choice.**
  GTK builds its AT-SPI backend only for X11 and Wayland
  (`gtk/gtkatcontext.c`), so under `GDK_BACKEND=broadway` the app starts fine,
  claims its bus name, and never appears on the a11y bus at all — you get an
  empty accessible tree and no clue why. `mutter --headless --no-x11
  --virtual-monitor 1600x1000` is the working headless display here; `xvfb` and
  `xdotool` are not installed and need root.
- **`GTK_A11Y=atspi` is not needed and, if the backend is unavailable, floods
  stderr** with one `Unrecognized accessibility backend` warning *per widget*.
  Leave it unset; the default picks AT-SPI when the display supports it.
- **A toggle button's lit state is AT-SPI `PRESSED`, not `CHECKED`.** GTK4 maps
  `GtkToggleButton:active` to `PRESSED`. Read only `CHECKED` and every format-bar
  button reports off forever, which looks exactly like a bug in the format bar.
- **`pick` on the first document is a no-op unless the selection is cleared
  first.** `GtkSingleSelection` already has row 0 selected before anything is
  asked of it, so selecting it emits no `selection-changed`, the binder never
  loads it, and the editor sits empty while the command reports success. The
  driver clears then selects; if you drive the tree yourself, do the same.
- **Trailing spaces in a driver script are significant.** `type - ` is a list
  being asked for and `type -` is a hyphen. The driver strips leading space and
  the newline only — use `\s` to make the intent visible.
- **`type` and `insert` are different gestures, not two spellings of one.** The
  autoformat and Enter-in-a-list rules key off a *lone* character arriving, so a
  one-shot `insert` of `- ` is deliberately not a list being asked for, and an
  `insert` containing a newline is a paste, not a return being pressed.
- **Picking a block style on an empty line then typing works everywhere except
  the buffer's last line.** A block tag covers its line's newline, and the last
  line has none for it to hold, so the pick has nothing to apply to and save
  drops the line. Type the words first, then pick the style — or leave a newline
  after the line. This is app behaviour, reproducible by hand, not a driver
  artifact.
- **Offsets are character offsets**, the unit `GtkTextIter` counts in — the same
  rule the undo records follow. Do not count bytes.
- **`expect` alone will pass a bug that only the author can see.** It reads what
  save wrote, and save regenerates list markers from the block tags — so a marker
  stranded in the middle of a line comes out of the file correct and off the
  screen wrong. Assert both: `shows` for the buffer, `expect` for the file.
- **`del` is not the Delete key.** AT-SPI's `DeleteText` calls
  `gtk_text_buffer_delete()`, which deletes non-editable text too, where the key
  goes through `delete_interactive()`, which steps over it. Both are worth
  driving and they are not the same path.
- **Frontmatter is not in the buffer.** `text` on `chapter-one` shows the body
  only; the `---` block is held aside verbatim and concatenated back on save.
  Assert on it with `expect`, which reads the file.
- **The app log goes to `/tmp`, not the project.** The binder is a directory
  scan, so a stray file at the project root would become a row in the UI under
  test. `logs` prints it.
- **`win.new-project` and `win.open-project` open a `GtkFileChooser`**, the one
  surface the driver cannot reach. Make projects with `new-project.sh` instead.
- **Keys that are not actions cannot be synthesised.** The deleting keys are
  caught at `GtkTextView::backspace` and `::delete-from-cursor` rather than as
  actions, so neither `take_back_autoformat()` nor the rule that takes a list off
  when a press runs into its marker is reachable this way; `do undo` is the same
  verb as the first and is. Both have headless coverage in `tests/ui/`, and the
  signal path itself can be driven from a throwaway program: a `GtkTextView` need
  not be realised to emit `delete-from-cursor`, so `g_signal_emit_by_name(view,
  "delete-from-cursor", GTK_DELETE_CHARS, 1)` over a buffer with marker tags in
  it exercises the real handlers with no display at all.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `no session bus — run this under dbus-run-session --` | Prefix the command with `dbus-run-session --`. |
| `no binary at .../wordsmith` | `cmake --build --preset debug -j`. |
| `mutter never came up` | A stale `mutter` from a killed run may hold the socket; `pkill -f wordsmith-drv` and retry. |
| `app never claimed io.github.dluco.Wordsmith` | Read the log path in the error — usually a missing GTK runtime dep. |
| `app never appeared on the a11y bus` | `GDK_BACKEND` got set to `broadway` somewhere; unset it. |
| Empty accessible tree, app clearly running | Same cause as above. |
| `Lost connection to Wayland compositor` at the end | Expected — that is teardown killing mutter after the app. Harmless. |
| Portal / keyring / `fusermount` noise on stderr | Expected from `dbus-run-session` starting a bare session. Filter it or ignore it. |
