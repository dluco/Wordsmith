#include "core/markup.hpp"

#include "core/markup-c.h"

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

/* Lists are serialized from the block's own `ordered` and `list_number`, never
 * from anything a caller wrote into a span. The editor used to draw its own
 * marker as well, and every save added one more — a manuscript that read
 * `- - - an item` after three sittings. */
void test_a_list_item_carries_exactly_one_marker()
{
    Document doc;

    Block bullet;
    bullet.kind = BlockKind::ListItem;
    bullet.spans.push_back(Span{"an item"});
    doc.blocks.push_back(bullet);

    Block ordered;
    ordered.kind        = BlockKind::ListItem;
    ordered.ordered     = true;
    ordered.list_number = 4;
    ordered.spans.push_back(Span{"the fourth"});
    doc.blocks.push_back(ordered);

    check_equal(wordsmith::markup::serialize(doc),
                "- an item\n\n4. the fourth\n",
                "list: one marker each, and the number the block carries");

    /* A bullet against a numbered item is two lists, not one, so the blank line
     * between them stays. Two items of the same kind still run together. */
    check_stable("- one\n- two\n\n1. first\n2. second\n",
                 "list: two lists stay two lists");
    check_equal(wordsmith::markup::serialize(
                    wordsmith::markup::parse("- one\n- two\n")),
                "- one\n- two\n", "list: items of one list run together");
}

/* The bridge is where the editor says all this, and the marker is deliberately
 * not sayable through it: a span holding `1. ` is what the doubling was. */
void test_the_builder_states_ordering_rather_than_drawing_it()
{
    WordsmithMarkupBuilder* builder = wordsmith_markup_builder_new();

    wordsmith_markup_builder_begin_block(builder, WORDSMITH_MARKUP_LIST_ITEM, 0);
    wordsmith_markup_builder_set_list(builder, 0, 1);
    wordsmith_markup_builder_add_span(builder, "an item", 0, nullptr);

    wordsmith_markup_builder_begin_block(builder, WORDSMITH_MARKUP_LIST_ITEM, 0);
    wordsmith_markup_builder_set_list(builder, 1, 1);
    wordsmith_markup_builder_add_span(builder, "first", 0, nullptr);

    wordsmith_markup_builder_begin_block(builder, WORDSMITH_MARKUP_LIST_ITEM, 0);
    wordsmith_markup_builder_set_list(builder, 1, 2);
    wordsmith_markup_builder_add_span(builder, "second", 0, nullptr);

    char* markdown = wordsmith_markup_builder_to_markdown(builder);
    check_equal(markdown, "- an item\n\n1. first\n2. second\n",
                "builder: markers come from the block, not from a span");
    wordsmith_free_string(markdown);
    wordsmith_markup_builder_free(builder);

    /* Saying it of something that is not a list item is ignored rather than
     * turning it into one, the way set_code() is. */
    builder = wordsmith_markup_builder_new();
    wordsmith_markup_builder_begin_block(builder, WORDSMITH_MARKUP_PARAGRAPH, 0);
    wordsmith_markup_builder_set_list(builder, 1, 3);
    wordsmith_markup_builder_add_span(builder, "prose", 0, nullptr);

    markdown = wordsmith_markup_builder_to_markdown(builder);
    check_equal(markdown, "prose\n", "builder: ordering is ignored off a list item");
    wordsmith_free_string(markdown);
    wordsmith_markup_builder_free(builder);

    /* And with nothing started at all, which must not reach past the end of an
     * empty block list. */
    builder = wordsmith_markup_builder_new();
    wordsmith_markup_builder_set_list(builder, 1, 1);
    markdown = wordsmith_markup_builder_to_markdown(builder);
    check_equal(markdown, "", "builder: ordering before any block is ignored");
    wordsmith_free_string(markdown);
    wordsmith_markup_builder_free(builder);
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
    test_a_list_item_carries_exactly_one_marker();
    test_the_builder_states_ordering_rather_than_drawing_it();
    test_empty();

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
