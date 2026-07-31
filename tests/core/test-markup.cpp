#include "core/markup.hpp"

#include <cassert>
#include <iostream>
#include <string>

using wordsmith::markup::Block;
using wordsmith::markup::BlockKind;
using wordsmith::markup::Document;
using wordsmith::markup::Span;

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        failures++;
    }
}

void check_equal(const std::string& actual, const std::string& expected,
                 const std::string& what)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << what << "\n  expected: [" << expected
                  << "]\n  actual:   [" << actual << "]\n";
        failures++;
    }
}

/* serialize(parse(x)) must be a fixed point: applying it twice changes
 * nothing. That is the property the editor's save path depends on. */
void check_stable(const std::string& markdown, const std::string& what)
{
    const std::string once = wordsmith::markup::serialize(
        wordsmith::markup::parse(markdown));
    const std::string twice = wordsmith::markup::serialize(
        wordsmith::markup::parse(once));
    check_equal(twice, once, what + " (round trip is not stable)");
}

void test_inline_styles()
{
    const std::vector<Span> spans =
        wordsmith::markup::parse_inline("plain **bold** and *italic*");

    check(spans.size() == 4, "inline: four runs");
    if (spans.size() != 4) {
        return;
    }
    check(spans[0].text == "plain " && !spans[0].strong, "inline: leading plain run");
    check(spans[1].text == "bold" && spans[1].strong, "inline: bold run");
    check(spans[2].text == " and ", "inline: middle plain run");
    check(spans[3].text == "italic" && spans[3].emphasis, "inline: italic run");
}

void test_underline()
{
    const std::vector<Span> spans =
        wordsmith::markup::parse_inline("a <u>marked</u> word");

    check(spans.size() == 3, "underline: three runs");
    if (spans.size() != 3) {
        return;
    }
    check(spans[1].text == "marked" && spans[1].underline, "underline: middle run");

    check_equal(wordsmith::markup::serialize_inline(spans),
                "a <u>marked</u> word", "underline: serializes back");
}

/* An autolink also opens on '<', so the two branches must not shadow one
 * another. */
void test_underline_does_not_eat_autolinks()
{
    const std::vector<Span> spans =
        wordsmith::markup::parse_inline("see <https://example.com> now");

    bool found_link = false;
    for (const Span& span : spans) {
        if (span.href == "https://example.com") {
            found_link = true;
        }
    }
    check(found_link, "autolink survives alongside the <u> branch");
}

void test_escaping()
{
    Document doc;
    Block block;
    Span span;
    span.text = "a * b _ c ` d [ e";
    block.spans.push_back(span);
    doc.blocks.push_back(block);

    const std::string markdown = wordsmith::markup::serialize(doc);
    const Document reparsed = wordsmith::markup::parse(markdown);

    check(reparsed.blocks.size() == 1, "escaping: one block survives");
    if (reparsed.blocks.size() != 1 || reparsed.blocks[0].spans.empty()) {
        return;
    }
    check_equal(reparsed.blocks[0].spans[0].text, "a * b _ c ` d [ e",
                "escaping: punctuation survives the round trip");
}

/* Prose is full of apostrophes and dashes; escaping those would make the .md
 * files unpleasant to read by hand. */
void test_prose_is_not_over_escaped()
{
    Document doc;
    Block block;
    Span span;
    span.text = "Don't stop (really) - it's fine.";
    block.spans.push_back(span);
    doc.blocks.push_back(block);

    check_equal(wordsmith::markup::serialize(doc),
                "Don't stop (really) - it's fine.\n",
                "prose: ordinary punctuation is left alone");
}

void test_heading_round_trip()
{
    const Document doc = wordsmith::markup::parse("## Chapter *One*\n");

    check(doc.blocks.size() == 1, "heading: one block");
    if (doc.blocks.empty()) {
        return;
    }
    check(doc.blocks[0].kind == BlockKind::Heading, "heading: kind");
    check(doc.blocks[0].level == 2, "heading: level");

    check_equal(wordsmith::markup::serialize(doc), "## Chapter *One*\n",
                "heading: serializes back");
}

void test_document_round_trip()
{
    const std::string source =
        "# The Long Winter\n"
        "\n"
        "The snow came early that year, and it came **hard**.\n"
        "\n"
        "## Chapter One\n"
        "\n"
        "She had *known* it would, of course. <u>Everyone</u> had.\n"
        "\n"
        "> There is no such thing as bad weather.\n"
        "\n"
        "- boots\n"
        "- lamp oil\n"
        "\n"
        "---\n";

    const Document doc = wordsmith::markup::parse(source);
    check(doc.blocks.size() == 8, "document: block count");

    check_stable(source, "document");
    check_equal(wordsmith::markup::serialize(doc), source,
                "document: serializes back byte for byte");
}

void test_code_block_round_trip()
{
    const std::string source = "```c\nint main(void) { return 0; }\n```\n";
    const Document doc = wordsmith::markup::parse(source);

    check(doc.blocks.size() == 1, "code: one block");
    if (doc.blocks.empty()) {
        return;
    }
    check(doc.blocks[0].kind == BlockKind::CodeBlock, "code: kind");
    check_equal(doc.blocks[0].language, "c", "code: language");
    check_equal(wordsmith::markup::serialize(doc), source, "code: serializes back");
}

/* A wrapped paragraph folds into one block, so the re-emitted line is longer
 * than the input. That is expected; what matters is that it then holds still. */
void test_wrapped_paragraph_folds_once()
{
    const std::string source = "The snow came early\nthat year.\n";
    check_equal(wordsmith::markup::serialize(wordsmith::markup::parse(source)),
                "The snow came early that year.\n",
                "wrapping: folds to a single line");
    check_stable(source, "wrapped paragraph");
}

void test_empty()
{
    check(wordsmith::markup::parse("").blocks.empty(), "empty: no blocks");
    check_equal(wordsmith::markup::serialize(Document{}), "",
                "empty: serializes to nothing");
}

} // namespace

int main()
{
    test_inline_styles();
    test_underline();
    test_underline_does_not_eat_autolinks();
    test_escaping();
    test_prose_is_not_over_escaped();
    test_heading_round_trip();
    test_document_round_trip();
    test_code_block_round_trip();
    test_wrapped_paragraph_folds_once();
    test_empty();

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
