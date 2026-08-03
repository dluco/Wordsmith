#!/bin/sh
# Make a throwaway Wordsmith project to drive. `new-project.sh <dir>`.
#
# The app can also create one itself (win.new-project), but that opens a file
# chooser, which is the one surface the driver cannot reach. A project is a
# directory with a JSON file and a manuscript folder, so making one by hand
# costs four lines and no dialog.
set -eu
root=${1:?usage: new-project.sh <dir>}
rm -rf "$root"
mkdir -p "$root/Manuscript"

cat > "$root/project.wordsmith" <<'JSON'
{
  "title": "Test",
  "manuscript": "Manuscript"
}
JSON

cat > "$root/Manuscript/chapter-one.md" <<'MD'
---
title: Chapter One
---

It was a dark and stormy night.
MD

cat > "$root/Manuscript/chapter-two.md" <<'MD'
---
title: Chapter Two
---

The rain fell in sheets.
MD

echo "$root"
