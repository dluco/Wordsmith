//
// YAML frontmatter: finding it, reading it, and changing one field of it
// without disturbing the rest.
//
// Detection is deliberately not a regular expression over the file. A `---` on
// the first line is also a perfectly good CommonMark thematic break (see
// `is_rule` in markup.cpp), so the two constructs are genuinely ambiguous in
// isolation. What disambiguates them is structure, and structure is what
// `scan` looks for:
//
//   1. the opening fence is at byte 0 — after an optional UTF-8 BOM and
//      nothing else. A leading blank line means there is no frontmatter,
//   2. that line is exactly `---`, unindented, trailing whitespace aside,
//   3. some later line is exactly `---` or `...`, likewise unindented,
//   4. no such closing line means this was never frontmatter, and the file is
//      Markdown from byte 0. This rule is what keeps a document that opens
//      with a thematic break from being swallowed whole,
//   5. what lies between the fences is YAML; what lies after is Markdown.
//
// `scan` yields byte ranges into the caller's buffer and rewrites nothing, so
// offsets stay usable for the editor's save path and for surgical field edits.
// Whether the YAML then parses is a separate question from whether frontmatter
// is present, and the two failures are reported separately.
//
// C++17, no GLib.
//

#ifndef WORDSMITH_FRONTMATTER_HPP
#define WORDSMITH_FRONTMATTER_HPP

#include "yaml.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wordsmith::frontmatter {

/**
 * Where a document's parts are. All offsets are into the scanned text.
 *
 * When `present` is false the whole document is Markdown, `body_begin` sits
 * after any BOM, and the YAML range is empty.
 *
 * text[0, body_begin) is the prologue: BOM, fences, and YAML together. The
 * editor keeps those bytes verbatim and puts them back on save, which is what
 * makes "we did not understand your YAML" a harmless condition rather than a
 * lossy one.
 */
struct Split {
    bool        present    = false;
    std::size_t yaml_begin = 0;  // first byte after the opening fence line
    std::size_t yaml_end   = 0;  // first byte of the closing fence line
    std::size_t body_begin = 0;  // first byte of the Markdown body
};

/** Locate the frontmatter fences. Structure only: no YAML is parsed, and this
 *  never fails — a document without frontmatter simply reports none. */
Split scan(std::string_view text);

/** A document split and its frontmatter parsed. `root` is an empty mapping
 *  when no frontmatter is present. */
struct Document {
    Split                         split;
    yaml::Node                    root;
    std::vector<yaml::Diagnostic> diagnostics;
};

/** Scan `text` and parse whatever frontmatter it has. Node byte ranges are in
 *  `text`'s coordinates, not the YAML region's. */
Document parse(std::string_view text);

/**
 * Return `text` with `key` set to `value`, or with `key` removed when `value`
 * is nullopt.
 *
 * Only the bytes belonging to that one field are touched: other fields keep
 * their order, their quoting, and any comments beside them, and the Markdown
 * body is untouched. Setting a key that is not there appends it before the
 * closing fence; setting one on a document with no frontmatter at all opens a
 * new block at the top. Removing a key that is not there changes nothing.
 *
 * `value` is a scalar. A value spanning lines is written as a block scalar; one
 * that would otherwise be misread as a number, a boolean, or a comment is
 * quoted. Setting a key whose current value is a sequence or a nested mapping
 * replaces the whole of it.
 */
std::string set_field(std::string_view text, std::string_view key,
                      std::optional<std::string_view> value);

} // namespace wordsmith::frontmatter

#endif /* WORDSMITH_FRONTMATTER_HPP */
