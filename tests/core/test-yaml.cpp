#include "core/yaml.hpp"

#include <iostream>
#include <string>

using wordsmith::yaml::Diagnostic;
using wordsmith::yaml::Node;

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

std::string scalar_of(const Node& root, const std::string& key)
{
    auto value = root.string(key);
    return value ? *value : std::string("<missing>");
}

void test_scalars()
{
    const Node root = wordsmith::yaml::parse(
        "title: The Wreck\n"
        "status: draft\n"
        "words: 4211\n");

    check_equal(scalar_of(root, "title"), "The Wreck", "scalar: plain value");
    check_equal(scalar_of(root, "status"), "draft", "scalar: second key");
    check(root.integer("words") == 4211, "scalar: integer");
    check(root.order.size() == 3 && root.order[0] == "title" && root.order[2] == "words",
          "scalar: keys keep document order");
}

void test_quotes_and_comments()
{
    const Node root = wordsmith::yaml::parse(
        "# a leading comment\n"
        "title: \"Quoted: with a colon\"\n"
        "note: plain # trailing comment\n"
        "hash: \"not # a comment\"\n"
        "apostrophe: 'it''s here'\n"
        "escaped: \"a \\\"quote\\\" inside\"\n");

    check_equal(scalar_of(root, "title"), "Quoted: with a colon", "quotes: colon inside");
    check_equal(scalar_of(root, "note"), "plain", "quotes: trailing comment stripped");
    check_equal(scalar_of(root, "hash"), "not # a comment", "quotes: hash inside quotes");
    check_equal(scalar_of(root, "apostrophe"), "it's here", "quotes: single-quote escape");
    check_equal(scalar_of(root, "escaped"), "a \"quote\" inside", "quotes: backslash escape");
}

void test_block_sequences()
{
    /* Items at the key's own indent, which is how frontmatter usually writes
     * tags and what Ogden's parser did not accept. */
    const Node flush = wordsmith::yaml::parse(
        "tags:\n"
        "- fiction\n"
        "- draft\n"
        "status: active\n");

    const Node* tags = flush.find("tags");
    check(tags != nullptr && tags->is_sequence() && tags->seq.size() == 2,
          "sequence: two items at the key's indent");
    if (tags != nullptr && tags->seq.size() == 2) {
        check_equal(tags->seq[0].scalar, "fiction", "sequence: first item");
        check_equal(tags->seq[1].scalar, "draft", "sequence: second item");
    }
    check_equal(scalar_of(flush, "status"), "active",
                "sequence: the key after it still parses");

    const Node indented = wordsmith::yaml::parse(
        "tags:\n"
        "  - fiction\n"
        "  - draft\n");
    const Node* nested = indented.find("tags");
    check(nested != nullptr && nested->is_sequence() && nested->seq.size() == 2,
          "sequence: indented items");
}

void test_flow_sequences()
{
    const Node root = wordsmith::yaml::parse(
        "tags: [fiction, draft, \"one, two\"]\n"
        "empty: []\n");

    const Node* tags = root.find("tags");
    check(tags != nullptr && tags->is_sequence() && tags->seq.size() == 3,
          "flow: three items");
    if (tags != nullptr && tags->seq.size() == 3) {
        check_equal(tags->seq[0].scalar, "fiction", "flow: first item");
        check_equal(tags->seq[2].scalar, "one, two", "flow: comma inside quotes");
    }
    const Node* empty = root.find("empty");
    check(empty != nullptr && empty->is_sequence() && empty->seq.empty(),
          "flow: empty sequence");
}

void test_block_scalars()
{
    const Node literal = wordsmith::yaml::parse(
        "synopsis: |\n"
        "  A ship founders.\n"
        "  Five nuns drown.\n"
        "status: draft\n");
    check_equal(scalar_of(literal, "synopsis"), "A ship founders.\nFive nuns drown.\n",
                "block: literal keeps newlines and clips to one");
    check_equal(scalar_of(literal, "status"), "draft",
                "block: the key after it still parses");

    const Node stripped = wordsmith::yaml::parse(
        "synopsis: |-\n"
        "  One line.\n"
        "  Another.\n");
    check_equal(scalar_of(stripped, "synopsis"), "One line.\nAnother.",
                "block: `-` strips the trailing newline");

    const Node folded = wordsmith::yaml::parse(
        "synopsis: >-\n"
        "  A long sentence\n"
        "  wrapped across lines.\n"
        "\n"
        "  A second paragraph.\n");
    check_equal(scalar_of(folded, "synopsis"),
                "A long sentence wrapped across lines.\nA second paragraph.",
                "block: folded joins lines and keeps the paragraph break");
}

void test_nested_map()
{
    const Node root = wordsmith::yaml::parse(
        "author:\n"
        "  name: Hopkins\n"
        "  born: 1844\n"
        "title: Poems\n");

    const Node* author = root.find("author");
    check(author != nullptr && author->is_map(), "nested: author is a map");
    if (author != nullptr) {
        check_equal(scalar_of(*author, "name"), "Hopkins", "nested: child scalar");
        check(author->integer("born") == 1844, "nested: child integer");
    }
    check_equal(scalar_of(root, "title"), "Poems", "nested: sibling after the block");
}

/* The ranges are what surgical editing rests on, so they get checked against
 * the exact bytes they are supposed to cover. */
void test_ranges()
{
    const std::string text =
        "title: The Wreck\n"
        "status: draft\n";
    const Node root = wordsmith::yaml::parse(text);

    const Node* title = root.find("title");
    check(title != nullptr, "range: title present");
    if (title != nullptr) {
        check_equal(text.substr(title->value.begin, title->value.size()), "The Wreck",
                    "range: value covers exactly the scalar");
        check_equal(text.substr(title->entry.begin, title->entry.size()),
                    "title: The Wreck\n", "range: entry covers the whole line");
    }

    const Node* status = root.find("status");
    if (status != nullptr) {
        check_equal(text.substr(status->entry.begin, status->entry.size()),
                    "status: draft\n", "range: second entry");
    }
}

void test_range_excludes_comment()
{
    const std::string text = "title: Wreck # the good one\n";
    const Node        root = wordsmith::yaml::parse(text);
    const Node*       title = root.find("title");
    check(title != nullptr, "range: commented entry present");
    if (title != nullptr) {
        check_equal(text.substr(title->value.begin, title->value.size()), "Wreck",
                    "range: value stops before the comment");
    }
}

void test_range_base_offset()
{
    const std::string document = "---\ntitle: Wreck\n---\n";
    const std::string region   = "title: Wreck\n";
    const Node        root     = wordsmith::yaml::parse(region, 4);
    const Node*       title    = root.find("title");
    check(title != nullptr, "range: base-offset entry present");
    if (title != nullptr) {
        check_equal(document.substr(title->value.begin, title->value.size()), "Wreck",
                    "range: base shifts offsets into the outer buffer");
    }
}

void test_empty_value()
{
    const std::string text = "title:\nstatus: draft\n";
    const Node        root = wordsmith::yaml::parse(text);
    const Node*       title = root.find("title");
    check(title != nullptr && title->is_scalar() && title->scalar.empty(),
          "empty: bare key is an empty scalar");
    if (title != nullptr) {
        check(title->value.empty(), "empty: value range is empty");
        check(text[title->value.begin - 1] == ':',
              "empty: value range sits against the colon");
    }
    check_equal(scalar_of(root, "status"), "draft", "empty: next key still parses");
}

void test_diagnostics()
{
    std::vector<Diagnostic> diagnostics;
    wordsmith::yaml::parse("title: ok\nthis line has no colon\n", 0, &diagnostics);
    check(diagnostics.size() == 1, "diagnostics: one complaint");
    if (diagnostics.size() == 1) {
        check(diagnostics[0].line == 2, "diagnostics: line number is 1-based");
    }
}

void test_encode_scalar()
{
    check_equal(wordsmith::yaml::encode_scalar("plain text", 0), "plain text",
                "encode: plain stays plain");
    check_equal(wordsmith::yaml::encode_scalar("true", 0), "\"true\"",
                "encode: a bool-looking string is quoted");
    check_equal(wordsmith::yaml::encode_scalar("42", 0), "\"42\"",
                "encode: a number-looking string is quoted");
    check_equal(wordsmith::yaml::encode_scalar("has: colon", 0), "\"has: colon\"",
                "encode: a colon forces quoting");
    check_equal(wordsmith::yaml::encode_scalar("- leading dash", 0),
                "\"- leading dash\"", "encode: a leading indicator forces quoting");
    check_equal(wordsmith::yaml::encode_scalar("", 0), "\"\"", "encode: empty is quoted");
    check_equal(wordsmith::yaml::encode_scalar("one\ntwo", 0), "|-\n  one\n  two",
                "encode: multi-line becomes a literal block");
    check_equal(wordsmith::yaml::encode_scalar("one\ntwo\n", 0), "|\n  one\n  two",
                "encode: a trailing newline becomes clip chomping");
}

/* Whatever encode_scalar writes, the parser has to read back unchanged. */
void check_round_trip(const std::string& value, const std::string& what)
{
    const std::string text = "key: " + wordsmith::yaml::encode_scalar(value, 0) + "\n";
    const Node        root = wordsmith::yaml::parse(text);
    check_equal(scalar_of(root, "key"), value, "round trip: " + what);
}

void test_encode_round_trip()
{
    check_round_trip("plain", "plain");
    check_round_trip("true", "bool-looking");
    check_round_trip("42", "number-looking");
    check_round_trip("has: colon", "colon");
    check_round_trip("trailing space kept ", "trailing space");
    check_round_trip("a \"quoted\" word", "embedded quotes");
    check_round_trip("one\ntwo", "multi-line stripped");
    check_round_trip("one\ntwo\n", "multi-line clipped");
    check_round_trip("# not a comment", "leading hash");
}

} // namespace

int main()
{
    test_scalars();
    test_quotes_and_comments();
    test_block_sequences();
    test_flow_sequences();
    test_block_scalars();
    test_nested_map();
    test_ranges();
    test_range_excludes_comment();
    test_range_base_offset();
    test_empty_value();
    test_diagnostics();
    test_encode_scalar();
    test_encode_round_trip();

    if (failures > 0) {
        std::cerr << failures << " yaml check(s) failed\n";
        return 1;
    }
    std::cout << "all yaml checks passed\n";
    return 0;
}
