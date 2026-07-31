//
// A small YAML reader for document frontmatter.
//
// Adapted from Ogden's ogden::core::detail::yaml-lite, which read `.clang-format`
// files. Frontmatter needs three things that version did not have, and they are
// the reason this is a port rather than a copy:
//
//   - byte ranges for every entry, so a field can be rewritten in place without
//     re-serializing (and so discarding) the rest of the file,
//   - block scalars (`|` and `>`), because a synopsis spanning several lines is
//     the most likely thing a writer puts in frontmatter,
//   - flow sequences (`[a, b]`), at least as common as `- a` for tags.
//
// Conversely, multi-document streams are gone: frontmatter is bounded by its
// fences before it reaches this parser, so a `---` can never appear at column 0
// inside the text handed here.
//
// This is NOT a general YAML parser. There are no anchors, aliases, tags, flow
// mappings, or explicit-key syntax, and nesting stops at two levels. Anything
// unrecognised is skipped with a diagnostic rather than guessed at.
//
// C++17, no GLib.
//

#ifndef WORDSMITH_YAML_HPP
#define WORDSMITH_YAML_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wordsmith::yaml {

/** A half-open byte range into the text that was parsed. */
struct Range {
    std::size_t begin = 0;
    std::size_t end   = 0;

    bool        empty() const noexcept { return begin >= end; }
    std::size_t size() const noexcept { return empty() ? 0 : end - begin; }
};

/** Something the parser could not make sense of. `line` is 1-based. */
struct Diagnostic {
    std::size_t line = 0;
    std::string message;
};

/**
 * One node. Scalars carry their decoded text; mappings and sequences carry
 * children. `entry` and `value` are byte ranges into the parsed text and are
 * what make surgical rewriting possible:
 *
 *   title: The Wreck of the Deutschland
 *   ^entry.begin
 *          ^value.begin                 ^value.end
 *                                        ^entry.end (past the newline)
 *
 * `entry` spans the whole `key: value` construct including its indent and
 * trailing newline, so erasing a field is a single splice. `value` spans just
 * the scalar text — no leading space, no trailing comment — so replacing a
 * field leaves the key, its indent, and any comment untouched. A block scalar's
 * `value` covers the indicator and every content line.
 *
 * A value written on the lines below its key — a block sequence, a nested
 * mapping — starts its range immediately after the colon and runs to the end of
 * the block, newline and indent included. Replacing such a field then has
 * somewhere to write, instead of landing on the line beneath the key.
 *
 * Both ranges are empty for the root node, which stands for the text as a
 * whole.
 */
class Node {
public:
    enum class Kind { Scalar, Map, Sequence };

    Kind                        kind = Kind::Scalar;
    std::string                 scalar;  // Kind::Scalar
    std::map<std::string, Node> map;     // Kind::Map, by key
    std::vector<Node>           seq;     // Kind::Sequence

    /* Keys in the order they appeared. `map` sorts, and the inspector shows
     * unrecognised fields in the order the writer wrote them. */
    std::vector<std::string> order;

    Range entry;
    Range value;

    bool is_scalar() const noexcept { return kind == Kind::Scalar; }
    bool is_map() const noexcept { return kind == Kind::Map; }
    bool is_sequence() const noexcept { return kind == Kind::Sequence; }

    /** Child of a mapping by key, or nullptr. */
    const Node* find(std::string_view key) const;

    /** Scalar string for `key`, or nullopt (key missing or not a scalar). */
    std::optional<std::string> string(std::string_view key) const;

    /** Scalar parsed as an integer, or nullopt. */
    std::optional<long> integer(std::string_view key) const;

    /** Scalar parsed as a YAML bool (true/yes/on and their negatives,
     *  case-insensitively), or nullopt. */
    std::optional<bool> boolean(std::string_view key) const;

    /** Insert or replace a child, keeping `order` in step. */
    void set(std::string key, Node child);
};

/**
 * Parse a block mapping.
 *
 * `base` is added to every recorded byte offset, so a caller holding a slice of
 * a larger buffer (frontmatter inside a document, say) gets ranges in the
 * coordinates of that larger buffer. `diagnostics`, when non-null, collects
 * what could not be parsed; the parse itself always succeeds, returning
 * whatever it did understand.
 */
Node parse(std::string_view text, std::size_t base = 0,
           std::vector<Diagnostic>* diagnostics = nullptr);

/**
 * Encode `value` as a scalar suitable for splicing over a Node's `value` range.
 * Multi-line values become a literal block scalar indented by `indent` + 2;
 * values that would otherwise be misread (leading punctuation, a trailing
 * comment, something that looks like a bool or a number) get quoted.
 */
std::string encode_scalar(std::string_view value, int indent);

} // namespace wordsmith::yaml

#endif /* WORDSMITH_YAML_HPP */
