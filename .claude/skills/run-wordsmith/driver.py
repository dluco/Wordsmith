#!/usr/bin/env python3
"""Drive a real, running Wordsmith headlessly.

Wordsmith is a GTK4 desktop app; there is no display in CI and no screenshot
step in this project's workflow. This driver launches the actual binary against
a headless `mutter` compositor and reaches into it two ways, both of which
answer in text:

  * the window's **GAction group over D-Bus** — every menu item, chord and
    format-bar button is one of these (`menu-bar.c` installs them), and the
    group reports each action's *enabled* flag and current state as well as
    taking activations. That is how a press is made and how "is Undo greyed
    out" is read.
  * the **AT-SPI tree** — the widget hierarchy with names, plus the editor's
    GtkTextView behind AT-SPI's EditableText interface, which is what lets
    text be inserted into the buffer and read back.

Commands come from stdin, one per line, results go to stdout. Run it under
`dbus-run-session` (both the a11y bus and the app's own bus name need a
session bus of their own). `help` lists the commands.

Why not X11: `xvfb`/`xdotool` are not installed here and installing them needs
root. `mutter --headless --virtual-monitor` is already present and gives a real
Wayland display, which is what GTK's AT-SPI backend requires — see the Gotchas
in SKILL.md for why Broadway, the obvious headless choice, cannot work.
"""

import atexit
import os
import shlex
import subprocess
import sys
import tempfile
import time

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, Gio, GLib  # noqa: E402

APP_ID = "io.github.dluco.Wordsmith"
# The seven `win.block-style` targets, from BLOCK_STYLES in ui/editor-panel.c.
# The names are what the dropdown shows; the ids are what the action takes.
BLOCK_STYLES = {
    "paragraph": "Paragraph",
    "heading-1": "Heading 1",
    "heading-2": "Heading 2",
    "heading-3": "Heading 3",
    "quote": "Block Quote",
    "bulleted-list": "Bulleted List",
    "numbered-list": "Numbered List",
}
BLOCK_STYLE_NAMES = set(BLOCK_STYLES.values())
WINDOW_PATH = "/io/github/dluco/Wordsmith/window/1"
REPO = os.path.dirname(  # .../run-wordsmith -> skills -> .claude -> repo
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
DEFAULT_BIN = os.path.join(REPO, "cmake-build-debug", "src", "driver", "wordsmith")

# The app has to be up before AT-SPI or D-Bus can see it, and mutter has to be
# up before the app. Both are polled rather than slept on, but these bound it.
LAUNCH_TIMEOUT = 25.0
SETTLE = 0.35  # after an action, before reading the result back


class Driver:
    def __init__(self, project, binary=None, verbose=True):
        self.project = os.path.abspath(project)
        self.binary = binary or os.environ.get("WORDSMITH_BIN", DEFAULT_BIN)
        self.verbose = verbose
        self.procs = []
        self.bus = None
        self.app = None
        self.rest = ""
        self.logpath = None
        self.failures = 0
        atexit.register(self.stop)

    # ---------------------------------------------------------------- launch

    def start(self):
        if not os.path.exists(self.binary):
            raise SystemExit(
                "no binary at %s — run `cmake --build --preset debug` first"
                % self.binary
            )
        if "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
            raise SystemExit("no session bus — run this under `dbus-run-session --`")

        display = "wordsmith-drv-%d" % os.getpid()
        self.procs.append(
            subprocess.Popen(
                [
                    "mutter",
                    "--headless",
                    "--no-x11",
                    "--wayland-display=" + display,
                    "--virtual-monitor",
                    "1600x1000",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        )
        self._await_socket(display)

        env = dict(os.environ, WAYLAND_DISPLAY=display, GDK_BACKEND="wayland")
        env.pop("DISPLAY", None)
        # Not inside the project: the binder is a directory scan, so a stray
        # file at the project root becomes a row in the UI under test.
        self.logpath = os.path.join(
            tempfile.gettempdir(), "wordsmith-driver-%d.log" % os.getpid()
        )
        self.applog = open(self.logpath, "wb")
        self.procs.append(
            subprocess.Popen(
                [self.binary, self.project],
                env=env,
                stdout=self.applog,
                stderr=subprocess.STDOUT,
            )
        )

        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self._await_bus_name()
        Atspi.init()
        self._await_atspi()
        self.say("# launched %s on %s" % (os.path.basename(self.project), display))

    def _await_socket(self, display):
        runtime = os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid())
        target = os.path.join(runtime, display)
        deadline = time.time() + LAUNCH_TIMEOUT
        while time.time() < deadline:
            if os.path.exists(target):
                time.sleep(0.5)  # mutter creates the socket a beat before it listens
                return
            time.sleep(0.2)
        raise SystemExit("mutter never came up (no %s)" % target)

    def _await_bus_name(self):
        deadline = time.time() + LAUNCH_TIMEOUT
        while time.time() < deadline:
            names = self.bus.call_sync(
                "org.freedesktop.DBus",
                "/org/freedesktop/DBus",
                "org.freedesktop.DBus",
                "ListNames",
                None,
                GLib.VariantType("(as)"),
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            ).unpack()[0]
            if APP_ID in names:
                return
            time.sleep(0.2)
        raise SystemExit("app never claimed %s — see %s" % (APP_ID, self.logpath))

    def _await_atspi(self):
        deadline = time.time() + LAUNCH_TIMEOUT
        while time.time() < deadline:
            if self._app_node() is not None:
                return
            time.sleep(0.3)
        raise SystemExit("app never appeared on the a11y bus")

    def stop(self):
        for proc in reversed(self.procs):
            if proc.poll() is None:
                proc.terminate()
        time.sleep(0.4)
        for proc in reversed(self.procs):
            if proc.poll() is None:
                proc.kill()
        self.procs = []

    # ------------------------------------------------------------- plumbing

    def say(self, *parts):
        print(*parts)
        sys.stdout.flush()

    def fail(self, message):
        self.failures += 1
        self.say("FAIL:", message)

    def _app_node(self):
        desktop = Atspi.get_desktop(0)
        for i in range(desktop.get_child_count()):
            node = desktop.get_child_at_index(i)
            try:
                if node and node.get_name() == "wordsmith":
                    return node
            except GLib.Error:
                continue
        return None

    def nodes(self):
        """Every accessible under the app, breadth-first."""
        out = []
        stack = [self._app_node()]
        while stack:
            node = stack.pop(0)
            if node is None:
                continue
            out.append(node)
            try:
                stack.extend(
                    node.get_child_at_index(i) for i in range(node.get_child_count())
                )
            except GLib.Error:
                pass
        return out

    def by_name(self, needle, role=None):
        for node in self.nodes():
            try:
                if needle.lower() in node.get_name().lower() and (
                    role is None or node.get_role_name() == role
                ):
                    return node
            except GLib.Error:
                continue
        return None

    def editor(self):
        """The manuscript GtkTextView — the one text node with EditableText."""
        for node in self.nodes():
            try:
                ifaces = Atspi.Accessible.get_interfaces(node)
                if "EditableText" in ifaces and node.get_role_name() == "text":
                    return node
            except GLib.Error:
                continue
        return None

    # -------------------------------------------------------------- actions

    def describe_all(self):
        return self.bus.call_sync(
            APP_ID,
            WINDOW_PATH,
            "org.gtk.Actions",
            "DescribeAll",
            None,
            GLib.VariantType("(a{s(bgav)})"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        ).unpack()[0]

    def activate(self, name, param=None):
        params = [param] if param is not None else []
        self.bus.call_sync(
            APP_ID,
            WINDOW_PATH,
            "org.gtk.Actions",
            "Activate",
            GLib.Variant("(sava{sv})", (name, params, {})),
            None,
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )
        time.sleep(SETTLE)

    # ------------------------------------------------------------- commands

    def cmd_help(self, _):
        self.say(__doc__.strip().splitlines()[0])
        for name in sorted(n[4:] for n in dir(self) if n.startswith("cmd_")):
            self.say("  " + name)

    def cmd_tree(self, args):
        depth = int(args[0]) if args else 99

        def walk(node, level):
            if node is None or level > depth:
                return
            try:
                ifaces = ",".join(Atspi.Accessible.get_interfaces(node))
                self.say(
                    "%s%s %r [%s]"
                    % ("  " * level, node.get_role_name(), node.get_name()[:60], ifaces)
                )
                for i in range(node.get_child_count()):
                    walk(node.get_child_at_index(i), level + 1)
            except GLib.Error:
                pass

        walk(self._app_node(), 0)

    def cmd_actions(self, args):
        """Every window action: name, enabled, param signature, state."""
        needle = args[0] if args else ""
        for name, (enabled, sig, state) in sorted(self.describe_all().items()):
            if needle and needle not in name:
                continue
            bits = ["enabled" if enabled else "DISABLED"]
            if sig:
                bits.append("param:" + sig)
            if state:
                bits.append("state=%r" % (state[0],))
            self.say("%-28s %s" % (name, " ".join(bits)))

    def cmd_do(self, args):
        """do <action> [string-param] — activate a window action."""
        name = args[0]
        param = GLib.Variant("s", args[1]) if len(args) > 1 else None
        self.activate(name, param)
        self.say("# do %s%s" % (name, " " + args[1] if len(args) > 1 else ""))

    def cmd_state(self, args):
        info = self.describe_all().get(args[0])
        if info is None:
            return self.fail("no action %r" % args[0])
        enabled, _sig, state = info
        self.say(
            "%s enabled=%s state=%s"
            % (args[0], enabled, state[0] if state else "<none>")
        )

    def cmd_rows(self, _):
        """Binder rows, in order. Each row's name is its filename."""
        for node in self.nodes():
            try:
                if node.get_role_name() == "list item":
                    label = node.get_child_at_index(0)
                    self.say("row %r" % (label.get_name() if label else ""))
            except GLib.Error:
                pass

    def cmd_pick(self, args):
        """pick <row-name> — select a binder row, which opens it."""
        needle = args[0]
        found = None
        for node in self.nodes():
            try:
                if node.get_role_name() != "list item":
                    continue
                label = node.get_child_at_index(0)
                if label and needle.lower() in label.get_name().lower():
                    found = (node, label.get_name())
                    break
            except GLib.Error:
                continue
        if found is None:
            return self.fail("no binder row matching %r" % needle)
        node, name = found
        # Selecting through the parent list is what the binder listens to;
        # do_action on the row is a click, which lands on the name label and
        # would open a rename on a row that was already selected.
        #
        # Clearing first is not tidiness: GtkSingleSelection has row 0 selected
        # before anything is asked of it, so `pick` on the first document of a
        # project would be a no-op, emit no selection-changed, and leave the
        # editor empty while reporting success.
        parent = node.get_parent()
        Atspi.Selection.clear_selection(parent)
        time.sleep(SETTLE)
        Atspi.Selection.select_child(parent, node.get_index_in_parent())
        time.sleep(SETTLE * 3)
        self.say("# picked %r" % name)

    def cmd_press(self, args):
        """press <widget-name> — invoke a button/toggle by its accessible name."""
        node = self.by_name(args[0])
        if node is None:
            return self.fail("no widget named ~%r" % args[0])
        Atspi.Action.do_action(node, 0)
        time.sleep(SETTLE)
        self.say("# pressed %r" % node.get_name())

    def cmd_buttons(self, _):
        """The format bar: what the block dropdown reads and which toggles are lit.

        These follow the text, never the click — a lit toggle is the editor
        reporting what is in force where the caret stands.
        """
        for node in self.nodes():
            try:
                role = node.get_role_name()
                name = node.get_name()
                if role == "combo box":
                    # The inspector has dropdowns too; only the format bar's
                    # reads a block style.
                    if name in BLOCK_STYLE_NAMES or name == "":
                        self.say("%-34s [block dropdown]" % name)
                elif role == "toggle button" and "(Ctrl" in name:
                    # GTK4 maps a GtkToggleButton's active flag to AT-SPI
                    # PRESSED, not CHECKED. Reading only CHECKED reports every
                    # button as off forever and looks exactly like a bug in the
                    # format bar.
                    states = Atspi.StateSet.get_states(node.get_state_set())
                    lit = (
                        Atspi.StateType.PRESSED in states
                        or Atspi.StateType.CHECKED in states
                    )
                    self.say("%-34s %s" % (name, "ON" if lit else "off"))
            except GLib.Error:
                pass

    def cmd_text(self, _):
        """The editor buffer, verbatim, with offsets an author would count in."""
        node = self.editor()
        if node is None:
            return self.fail("no editor text view")
        count = Atspi.Text.get_character_count(node)
        self.say("--- editor (%d chars) ---" % count)
        self.say(Atspi.Text.get_text(node, 0, count))
        self.say("--- end ---")

    def cmd_type(self, args):
        """type <text> — insert one character at a time, as a keyboard would.

        This matters: the autoformat and Enter-in-a-list rules both key off a
        *lone* character arriving, so a one-shot insert of "- " is explicitly
        not a list being asked for. Use `insert` for the paste-like path.
        """
        node = self.editor()
        if node is None:
            return self.fail("no editor text view")
        text = self._unescape(self.rest)
        for char in text:
            offset = Atspi.Text.get_caret_offset(node)
            Atspi.EditableText.insert_text(node, offset, char, len(char.encode()))
            time.sleep(0.05)
        self.say("# typed %r" % text)

    def cmd_insert(self, args):
        """insert <text> — one insertion, the paste path."""
        node = self.editor()
        if node is None:
            return self.fail("no editor text view")
        text = self._unescape(self.rest)
        offset = Atspi.Text.get_caret_offset(node)
        Atspi.EditableText.insert_text(node, offset, text, len(text.encode()))
        time.sleep(SETTLE)
        self.say("# inserted %r" % text)

    def cmd_caret(self, args):
        node = self.editor()
        if args[0] == "end":
            where = Atspi.Text.get_character_count(node)
        else:
            where = int(args[0])
        Atspi.Text.set_caret_offset(node, where)
        time.sleep(SETTLE)
        self.say("# caret %d" % where)

    def cmd_sel(self, args):
        node = self.editor()
        Atspi.Text.add_selection(node, int(args[0]), int(args[1]))
        time.sleep(SETTLE)
        self.say("# selected %s..%s" % (args[0], args[1]))

    def cmd_del(self, args):
        node = self.editor()
        Atspi.EditableText.delete_text(node, int(args[0]), int(args[1]))
        time.sleep(SETTLE)
        self.say("# deleted %s..%s" % (args[0], args[1]))

    def cmd_sh(self, args):
        """sh <command> — run a shell command, cwd is the project directory."""
        out = subprocess.run(
            self.rest,  # raw, so the shell sees the quoting as written
            shell=True,
            cwd=self.project,
            capture_output=True,
            text=True,
        )
        self.say(out.stdout.rstrip())
        if out.stderr.strip():
            self.say("(stderr) " + out.stderr.rstrip())

    def cmd_shows(self, args):
        """shows <substring> — assert the editor buffer contains it.

        The counterpart to `expect`, and not the same question: `expect` reads
        what save wrote, and a bug that leaves the manuscript right and the
        screen wrong passes it. A stranded list marker is exactly that — save
        skips marker-tagged text, so the file came out correct while the author
        was looking at a stray `- ` in the middle of a line.
        """
        node = self.editor()
        if node is None:
            return self.fail("no editor text view")
        needle = self._unescape(self.rest)
        body = Atspi.Text.get_text(node, 0, Atspi.Text.get_character_count(node))
        if needle in body:
            self.say("ok  editor shows %r" % needle)
        else:
            self.fail("editor does not show %r\n--- actual ---\n%s" % (needle, body))

    def cmd_expect(self, args):
        """expect <file> <substring> — assert a file in the project contains it."""
        path = os.path.join(self.project, args[0])
        needle = self._unescape(" ".join(args[1:]))
        try:
            body = open(path).read()
        except OSError as exc:
            return self.fail("expect %s: %s" % (args[0], exc))
        if needle in body:
            self.say("ok  %s contains %r" % (args[0], needle))
        else:
            self.fail("%s does not contain %r\n--- actual ---\n%s" % (args[0], needle, body))

    def cmd_logs(self, _):
        """The app's own stdout/stderr — GTK warnings land here."""
        self.applog.flush()
        self.say(open(self.logpath).read().rstrip() or "(no output)")

    def cmd_wait(self, args):
        time.sleep(float(args[0]) if args else 1.0)

    def cmd_quit(self, _):
        raise EOFError

    @staticmethod
    def _unescape(text):
        # `\s` is here so a script can say "a space, and I meant it" without
        # relying on the reader to notice a trailing space in a heredoc.
        return (
            text.replace("\\n", "\n").replace("\\t", "\t").replace("\\s", " ")
        )

    # ----------------------------------------------------------------- repl

    def run(self, stream):
        for raw in stream:
            # Only leading space and the newline go. A *trailing* space is the
            # whole of the autoformat gesture — `type - ` is a list being asked
            # for and `type -` is a hyphen — so stripping the line would quietly
            # turn the most interesting command in the set into a different one.
            line = raw.lstrip().rstrip("\r\n")
            if not line.strip() or line.startswith("#"):
                if line:
                    self.say(line)
                continue
            word, _, rest = line.partition(" ")
            # Text-bearing commands keep the raw remainder: shlex would eat the
            # backslash in `type foo\nbar`, and quoting every space is worse.
            self.rest = rest
            parts = [word] + (shlex.split(rest) if rest else [])
            handler = getattr(self, "cmd_" + parts[0], None)
            if handler is None:
                self.fail("unknown command %r (try `help`)" % parts[0])
                continue
            try:
                handler(parts[1:])
            except EOFError:
                break
            except Exception as exc:  # a driver crash must not look like a pass
                self.fail("%s: %s: %s" % (parts[0], type(exc).__name__, exc))


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: driver.py <project-dir> [script]  (commands on stdin)")
    driver = Driver(sys.argv[1])
    driver.start()
    stream = open(sys.argv[2]) if len(sys.argv) > 2 else sys.stdin
    driver.run(stream)
    driver.stop()
    if driver.failures:
        driver.say("\n%d FAILURE(S)" % driver.failures)
        return 1
    driver.say("\nall ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
