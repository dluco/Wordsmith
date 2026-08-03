//
// A small, UI-agnostic Markdown model, parser and serializer.
//
// Adapted from Ogden's ogden::markup, which parsed LSP hover text for display.
// Wordsmith also has to write Markdown back out, so this version gains a
// serializer and an underline style (Ogden had no need for either).
//
// C++17, no GLib.
//

#ifndef WORDSMITH_MARKUP_HPP
#define WORDSMITH_MARKUP_HPP

#include <string>
#include <string_view>
#include <vector>

namespace wordsmith::markup {

/**
 * One styled run of inline text. Styles are flat (not a nested tree) so the run
 * maps directly onto GtkTextBuffer tags. `href` non-empty marks the run as a
 * link (its visible text is `text`).
 */
struct Span {
    std::string text;
    bool        emphasis  = false;  // *italic*
    bool        strong    = false;  // **bold**
    bool        underline = false;  // <u>underline</u>
    bool        code      = false;  // `monospace`
    std::string href;               // link target, empty if not a link
};

enum class BlockKind { Paragraph, Heading, CodeBlock, ListItem, Quote, Rule };

/**
 * A block-level element. `spans` carry the content of Paragraph/Heading/
 * ListItem/Quote; CodeBlock uses `code`/`language`; Rule carries nothing.
 */
struct Block {
    BlockKind         kind        = BlockKind::Paragraph;
    int               level       = 0;       // heading level, or list nesting depth
    bool              ordered     = false;   // ListItem: ordered vs bullet
    int               list_number = 0;       // ordered ListItem number
    std::string       code;                  // CodeBlock body
    std::string       language;              // CodeBlock language (may be empty)
    std::vector<Span> spans;
};

struct Document {
    std::vector<Block> blocks;
};

/** Parse a Markdown string (CommonMark subset) into a Document. Lenient:
 *  unrecognised constructs degrade to paragraph text rather than failing. */
Document parse(std::string_view markdown);

/** Parse a single line of inline Markdown into styled spans. Exposed for
 *  testing; `parse` uses it internally. */
std::vector<Span> parse_inline(std::string_view text);

/** Render a Document back to Markdown, blocks separated by a blank line.
 *
 *  This is not byte-for-byte lossless against arbitrary input. `parse` folds a
 *  wrapped paragraph into one block, so serializing re-emits it as a single
 *  long line, and marker styles are normalised (`-` for bullets, `*` for
 *  emphasis). Round-tripping is stable, though: serialize(parse(x)) reaches a
 *  fixed point after one pass.
 *
 *  **A list item's marker is written here**, from the block's own `ordered` and
 *  `list_number`. A caller that also puts one in a span gets both, and gets
 *  another on every save after that. Items of one list run together; a bullet
 *  item against a numbered one is two lists, so the blank line between them
 *  stays. */
std::string serialize(const Document& document);

/** Render a single run of spans as inline Markdown, escaping as needed.
 *  Exposed for testing; `serialize` uses it internally. */
std::string serialize_inline(const std::vector<Span>& spans);

} // namespace wordsmith::markup

#endif /* WORDSMITH_MARKUP_HPP */
