#include "core/frontmatter.hpp"

#include <iostream>
#include <string>

using wordsmith::frontmatter::Document;
using wordsmith::frontmatter::Split;

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

std::string body_of(const std::string& text)
{
    return text.substr(wordsmith::frontmatter::scan(text).body_begin);
}

std::string yaml_of(const std::string& text)
{
    const Split split = wordsmith::frontmatter::scan(text);
    return text.substr(split.yaml_begin, split.yaml_end - split.yaml_begin);
}

/* ── detection ──────────────────────────────────────────────────────────── */

void test_basic_split()
{
    const std::string text =
        "---\n"
        "title: The Wreck\n"
        "---\n"
        "# Chapter One\n"
        "\n"
        "It began at sea.\n";

    const Split split = wordsmith::frontmatter::scan(text);
    check(split.present, "split: frontmatter found");
    check_equal(yaml_of(text), "title: The Wreck\n", "split: yaml region");
    check_equal(body_of(text), "# Chapter One\n\nIt began at sea.\n", "split: body");
}

/* Rule 4, and the case the whole design turns on: without a closing fence the
 * `---` is a thematic break, not the start of frontmatter. */
void test_unclosed_fence_is_not_frontmatter()
{
    const std::string text =
        "---\n"
        "\n"
        "A document that opens with a horizontal rule.\n";

    const Split split = wordsmith::frontmatter::scan(text);
    check(!split.present, "unclosed: not frontmatter");
    check(split.body_begin == 0, "unclosed: body starts at byte 0");
    check_equal(body_of(text), text, "unclosed: whole file is the body");
}

void test_fence_only()
{
    check(!wordsmith::frontmatter::scan("---").present, "fence only: no newline");
    check(!wordsmith::frontmatter::scan("---\n").present, "fence only: with newline");
}

void test_leading_blank_line()
{
    const std::string text = "\n---\ntitle: x\n---\nBody\n";
    check(!wordsmith::frontmatter::scan(text).present,
          "leading blank: fence must be at byte 0");
}

void test_indented_fence()
{
    /* Four spaces make it an indented code block, not a fence. */
    const std::string text = "    ---\ntitle: x\n    ---\nBody\n";
    check(!wordsmith::frontmatter::scan(text).present, "indented: not a fence");

    const std::string one_space = " ---\ntitle: x\n---\nBody\n";
    check(!wordsmith::frontmatter::scan(one_space).present,
          "indented: even one space disqualifies the opener");
}

void test_longer_rule_is_not_a_fence()
{
    const std::string text = "----\ntitle: x\n----\nBody\n";
    check(!wordsmith::frontmatter::scan(text).present, "four hyphens: not a fence");
}

void test_dot_terminator()
{
    const std::string text = "---\ntitle: x\n...\nBody\n";
    const Split       split = wordsmith::frontmatter::scan(text);
    check(split.present, "dots: `...` closes the block");
    check_equal(body_of(text), "Body\n", "dots: body after the terminator");
}

void test_crlf()
{
    const std::string text = "---\r\ntitle: The Wreck\r\n---\r\nBody\r\n";
    const Split       split = wordsmith::frontmatter::scan(text);
    check(split.present, "crlf: fences match with carriage returns");
    check_equal(body_of(text), "Body\r\n", "crlf: body");

    const Document document = wordsmith::frontmatter::parse(text);
    check(document.root.string("title").value_or("") == "The Wreck",
          "crlf: value has no stray carriage return");
}

void test_bom()
{
    const std::string text = "\xEF\xBB\xBF---\ntitle: x\n---\nBody\n";
    const Split       split = wordsmith::frontmatter::scan(text);
    check(split.present, "bom: fence found after the BOM");
    check_equal(body_of(text), "Body\n", "bom: body");

    const std::string plain = "\xEF\xBB\xBF# Heading\n";
    const Split       none  = wordsmith::frontmatter::scan(plain);
    check(!none.present && none.body_begin == 3,
          "bom: body starts after the BOM when there is no frontmatter");
}

void test_empty_frontmatter()
{
    const std::string text  = "---\n---\nBody\n";
    const Split       split = wordsmith::frontmatter::scan(text);
    check(split.present, "empty: present");
    check(split.yaml_begin == split.yaml_end, "empty: yaml region is empty");
    check_equal(body_of(text), "Body\n", "empty: body");
}

void test_frontmatter_only()
{
    const std::string text  = "---\ntitle: x\n---\n";
    const Split       split = wordsmith::frontmatter::scan(text);
    check(split.present, "only: present");
    check(split.body_begin == text.size(), "only: body is empty");
    check_equal(body_of(text), "", "only: nothing after the fence");
}

void test_no_trailing_newline_on_close()
{
    const std::string text  = "---\ntitle: x\n---";
    const Split       split = wordsmith::frontmatter::scan(text);
    check(split.present, "no trailing newline: still closes");
    check(split.body_begin == text.size(), "no trailing newline: empty body");
}

/* A `---` inside the YAML would have to be indented to be a value, and an
 * unindented one closes the block — so the first one always wins. */
void test_rule_in_body_is_left_alone()
{
    const std::string text =
        "---\n"
        "title: x\n"
        "---\n"
        "Body above.\n"
        "\n"
        "---\n"
        "\n"
        "Body below.\n";
    check_equal(body_of(text), "Body above.\n\n---\n\nBody below.\n",
                "body rule: a later `---` stays in the body");
}

void test_body_offset_of_plain_markdown()
{
    const std::string text  = "# Just Markdown\n";
    const Split       split = wordsmith::frontmatter::scan(text);
    check(!split.present && split.body_begin == 0, "plain: body is the whole file");
}

/* ── parsing ────────────────────────────────────────────────────────────── */

void test_parse_fields()
{
    const std::string text =
        "---\n"
        "title: The Wreck of the Deutschland\n"
        "status: draft\n"
        "tags:\n"
        "  - poetry\n"
        "  - shipwreck\n"
        "synopsis: |-\n"
        "  A ship founders.\n"
        "  Five nuns drown.\n"
        "---\n"
        "Body.\n";

    const Document document = wordsmith::frontmatter::parse(text);
    check(document.diagnostics.empty(), "parse: no diagnostics");
    check_equal(document.root.string("title").value_or(""),
                "The Wreck of the Deutschland", "parse: title");
    check_equal(document.root.string("synopsis").value_or(""),
                "A ship founders.\nFive nuns drown.", "parse: block synopsis");

    const auto* tags = document.root.find("tags");
    check(tags != nullptr && tags->is_sequence() && tags->seq.size() == 2,
          "parse: tags sequence");
    check(document.root.order.size() == 4, "parse: four keys in order");

    /* Ranges must land in the whole document's coordinates, not the region's. */
    const auto* title = document.root.find("title");
    if (title != nullptr) {
        check_equal(text.substr(title->value.begin, title->value.size()),
                    "The Wreck of the Deutschland", "parse: value range is absolute");
    }
}

void test_parse_without_frontmatter()
{
    const Document document = wordsmith::frontmatter::parse("# Heading\n");
    check(!document.split.present, "parse: absent");
    check(document.root.is_map() && document.root.order.empty(),
          "parse: empty mapping when absent");
}

void test_malformed_yaml_still_splits()
{
    const std::string text =
        "---\n"
        "title: fine\n"
        "this line makes no sense\n"
        "---\n"
        "Body.\n";

    const Document document = wordsmith::frontmatter::parse(text);
    check(document.split.present, "malformed: the split still holds");
    check_equal(body_of(text), "Body.\n", "malformed: body is still the body");
    check_equal(document.root.string("title").value_or(""), "fine",
                "malformed: the good field still parses");
    check(document.diagnostics.size() == 1, "malformed: one diagnostic");
    if (document.diagnostics.size() == 1) {
        check(document.diagnostics[0].line == 3,
              "malformed: diagnostic line counts from the document, not the region");
    }
}

/* ── surgical editing ───────────────────────────────────────────────────── */

void test_set_existing_field()
{
    const std::string text =
        "---\n"
        "title: Old Title\n"
        "status: draft\n"
        "---\n"
        "Body.\n";

    check_equal(wordsmith::frontmatter::set_field(text, "title", "New Title"),
                "---\n"
                "title: New Title\n"
                "status: draft\n"
                "---\n"
                "Body.\n",
                "set: replaces the value in place");
}

/* The point of the whole ranges design: everything the edit did not name comes
 * through untouched, comments and quoting included. */
void test_set_preserves_everything_else()
{
    const std::string text =
        "---\n"
        "# Metadata for this chapter.\n"
        "zebra:   'quoted oddly'\n"
        "title: Old   # keep this comment\n"
        "apple: [a, b]\n"
        "\n"
        "status: draft\n"
        "---\n"
        "Body.\n";

    check_equal(wordsmith::frontmatter::set_field(text, "title", "New"),
                "---\n"
                "# Metadata for this chapter.\n"
                "zebra:   'quoted oddly'\n"
                "title: New   # keep this comment\n"
                "apple: [a, b]\n"
                "\n"
                "status: draft\n"
                "---\n"
                "Body.\n",
                "set: comments, order, spacing, and quoting all survive");
}

void test_set_empty_value_field()
{
    check_equal(wordsmith::frontmatter::set_field("---\ntitle:\n---\nBody.\n",
                                                  "title", "Something"),
                "---\ntitle: Something\n---\nBody.\n",
                "set: a bare key gains its separating space");
}

void test_set_new_field()
{
    check_equal(wordsmith::frontmatter::set_field("---\ntitle: x\n---\nBody.\n",
                                                  "status", "draft"),
                "---\ntitle: x\nstatus: draft\n---\nBody.\n",
                "set: a new key is appended above the closing fence");
}

void test_set_creates_frontmatter()
{
    check_equal(wordsmith::frontmatter::set_field("# Heading\n", "title", "New"),
                "---\ntitle: New\n---\n# Heading\n",
                "set: a block is opened when there is none");

    check_equal(wordsmith::frontmatter::set_field("\xEF\xBB\xBF# Heading\n", "title", "New"),
                "\xEF\xBB\xBF---\ntitle: New\n---\n# Heading\n",
                "set: a new block goes after the BOM");
}

void test_set_multiline_value()
{
    check_equal(wordsmith::frontmatter::set_field("---\nsynopsis: short\n---\nBody.\n",
                                                  "synopsis", "One line.\nTwo lines."),
                "---\nsynopsis: |-\n  One line.\n  Two lines.\n---\nBody.\n",
                "set: a multi-line value becomes a block scalar");
}

void test_set_over_a_block_scalar()
{
    const std::string text =
        "---\n"
        "synopsis: |-\n"
        "  One line.\n"
        "  Two lines.\n"
        "status: draft\n"
        "---\n"
        "Body.\n";

    check_equal(wordsmith::frontmatter::set_field(text, "synopsis", "Short now."),
                "---\nsynopsis: Short now.\nstatus: draft\n---\nBody.\n",
                "set: replacing a block scalar removes all of its lines");
}

void test_set_over_a_sequence()
{
    const std::string text = "---\ntags:\n  - a\n  - b\nstatus: draft\n---\nBody.\n";
    check_equal(wordsmith::frontmatter::set_field(text, "tags", "solo"),
                "---\ntags: solo\nstatus: draft\n---\nBody.\n",
                "set: replacing a sequence removes all of its items");
}

/* Tags arrive from the inspector as a list, and a document that has never had
 * frontmatter is the common case for the first one written. */
void test_set_sequence()
{
    check_equal(wordsmith::frontmatter::set_sequence(
                    "---\ntitle: x\n---\nBody.\n", "tags", {"one", "two"}),
                "---\ntitle: x\ntags:\n  - one\n  - two\n---\nBody.\n",
                "set sequence: appended to an existing block");

    check_equal(wordsmith::frontmatter::set_sequence("Body.\n", "tags", {"one"}),
                "---\ntags:\n  - one\n---\nBody.\n",
                "set sequence: opens a block when there is none");

    check_equal(wordsmith::frontmatter::set_sequence(
                    "---\ntags:\n  - old\nstatus: draft\n---\nBody.\n", "tags",
                    {"new"}),
                "---\ntags:\n  - new\nstatus: draft\n---\nBody.\n",
                "set sequence: replaces every item and keeps what follows");

    check_equal(wordsmith::frontmatter::set_sequence(
                    "---\ntitle: x\n---\nBody.\n", "tags", {}),
                "---\ntitle: x\ntags: []\n---\nBody.\n",
                "set sequence: no items is written as deliberately none");
}

void test_erase_field()
{
    check_equal(wordsmith::frontmatter::set_field(
                    "---\ntitle: x\nstatus: draft\n---\nBody.\n", "status", std::nullopt),
                "---\ntitle: x\n---\nBody.\n", "erase: removes the whole entry");

    check_equal(wordsmith::frontmatter::set_field(
                    "---\ntags:\n  - a\n  - b\nstatus: draft\n---\nBody.\n", "tags",
                    std::nullopt),
                "---\nstatus: draft\n---\nBody.\n",
                "erase: removes every line of a sequence");
}

void test_erase_missing_field_is_a_no_op()
{
    const std::string text = "---\ntitle: x\n---\nBody.\n";
    check_equal(wordsmith::frontmatter::set_field(text, "absent", std::nullopt), text,
                "erase: a missing key changes nothing");
    check_equal(wordsmith::frontmatter::set_field("# Plain\n", "absent", std::nullopt),
                "# Plain\n", "erase: no frontmatter changes nothing");
}

/* set_field then parse has to give back exactly what was set, for the values
 * most likely to trip the encoder. */
void check_set_round_trip(const std::string& value, const std::string& what)
{
    const std::string edited =
        wordsmith::frontmatter::set_field("---\ntitle: x\n---\nBody.\n", "title", value);
    const Document document = wordsmith::frontmatter::parse(edited);
    check_equal(document.root.string("title").value_or("<missing>"), value,
                "set round trip: " + what);
    check_equal(edited.substr(document.split.body_begin), "Body.\n",
                "set round trip: body intact for " + what);
}

void test_set_round_trip()
{
    check_set_round_trip("Plain Title", "plain");
    check_set_round_trip("A Title: With a Colon", "colon");
    check_set_round_trip("42", "number-looking");
    check_set_round_trip("no", "bool-looking");
    check_set_round_trip("- starts with a dash", "leading indicator");
    check_set_round_trip("has # a hash", "hash");
    check_set_round_trip("line one\nline two", "multi-line");
}

} // namespace

int main()
{
    test_basic_split();
    test_unclosed_fence_is_not_frontmatter();
    test_fence_only();
    test_leading_blank_line();
    test_indented_fence();
    test_longer_rule_is_not_a_fence();
    test_dot_terminator();
    test_crlf();
    test_bom();
    test_empty_frontmatter();
    test_frontmatter_only();
    test_no_trailing_newline_on_close();
    test_rule_in_body_is_left_alone();
    test_body_offset_of_plain_markdown();

    test_parse_fields();
    test_parse_without_frontmatter();
    test_malformed_yaml_still_splits();

    test_set_existing_field();
    test_set_preserves_everything_else();
    test_set_empty_value_field();
    test_set_new_field();
    test_set_creates_frontmatter();
    test_set_multiline_value();
    test_set_over_a_block_scalar();
    test_set_over_a_sequence();
    test_set_sequence();
    test_erase_field();
    test_erase_missing_field_is_a_no_op();
    test_set_round_trip();

    if (failures > 0) {
        std::cerr << failures << " frontmatter check(s) failed\n";
        return 1;
    }
    std::cout << "all frontmatter checks passed\n";
    return 0;
}
