#include "core/spelling.hpp"

#include <iostream>
#include <string>
#include <vector>

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
        std::cerr << "FAIL: " << what << "\n  expected: " << expected
                  << "\n  actual:   " << actual << '\n';
        failures++;
    }
}

void check_equal(std::size_t actual, std::size_t expected, const std::string& what)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << what << "\n  expected: " << expected
                  << "\n  actual:   " << actual << '\n';
        failures++;
    }
}

/* The words of `text`, run together with spaces, which is what most of these
 * are actually asking about. */
std::string words_of(std::string_view text)
{
    std::string out;
    for (const wordsmith::Word& word : wordsmith::words_in(text)) {
        if (!out.empty()) {
            out += ' ';
        }
        out += word.text;
    }
    return out;
}

/* The same, with the dictionary's own list of what may sit inside a word. */
std::string words_of(std::string_view text, std::string_view extra_word_chars)
{
    std::string out;
    for (const wordsmith::Word& word : wordsmith::words_in(text, extra_word_chars)) {
        if (!out.empty()) {
            out += ' ';
        }
        out += word.text;
    }
    return out;
}

/* ── what counts as a word ──────────────────────────────────────────────── */

void test_plain_prose()
{
    check_equal(words_of("The quick brown fox"), "The quick brown fox",
                "a plain sentence is its words");
    check_equal(words_of(""), "", "empty text has no words");
    check_equal(words_of("   \n\t "), "", "whitespace has no words");
    check_equal(words_of("...!?"), "", "punctuation alone has no words");
}

/* An apostrophe inside a word belongs to it; one at either end is quotation. */
void test_apostrophes()
{
    check_equal(words_of("don't"), "don't", "an apostrophe holds a word together");
    check_equal(words_of("O'Brien"), "O'Brien", "and does so mid-name");
    check_equal(words_of("'tis"), "tis", "a leading apostrophe is trimmed");
    check_equal(words_of("cats'"), "cats", "a trailing apostrophe is trimmed");
    check_equal(words_of("'quoted'"), "quoted", "a quoted word is just the word");

    /* The typographic apostrophe does the same job, because a manuscript
     * written anywhere else arrives full of them. */
    check_equal(words_of("don’t"), "don’t",
                "a typographic apostrophe holds a word together too");
    check_equal(words_of("‘quoted’"), "quoted",
                "typographic quotes are trimmed");
}

/* Dictionaries hold the halves, not the compound. */
void test_hyphens_break_words()
{
    check_equal(words_of("well-known"), "well known", "a hyphen breaks a word");
    check_equal(words_of("mother-in-law"), "mother in law", "and breaks it again");
}

/* Dashes are not hyphens: an em dash between two words is a break, and losing
 * that would make one unspellable word out of two spelled ones. */
void test_dashes_and_punctuation_break_words()
{
    check_equal(words_of("said—a"), "said a", "an em dash breaks a word");
    check_equal(words_of("here–there"), "here there", "so does an en dash");
    check_equal(words_of("wait… what"), "wait what", "so does an ellipsis");
    check_equal(words_of("“Hello,” she said"), "Hello she said",
                "curly quotes are not part of the words they hold");
}

/* Nothing with a digit in it is a spelling mistake. */
void test_anything_with_a_digit_is_left_alone()
{
    check_equal(words_of("1984"), "", "a year is not offered");
    check_equal(words_of("3rd"), "", "nor an ordinal");
    check_equal(words_of("v2 and Q4"), "and", "nor a version or a quarter");
    check_equal(words_of("chapter 12 opens"), "chapter opens",
                "the words around a number still are");
}

/* Accented letters hold a word together without a Unicode table in the build,
 * which is the whole point of the rule that everything above ASCII is a letter
 * unless it is named punctuation. */
void test_letters_above_ascii()
{
    check_equal(words_of("café naïve"), "café naïve",
                "accented letters belong to their words");
    check_equal(words_of("Æsop"), "Æsop", "and so do the ligatures");
    check_equal(words_of("αβγ"), "αβγ",
                "a Greek word is a word");
    check_equal(words_of("2 × 3"), "", "but × is a symbol, not a letter");
}

/* ── where a word is ────────────────────────────────────────────────────── */

/* Offsets count characters, not bytes, because the only caller places
 * GtkTextIters with them. A byte offset works until the first accented
 * character and then puts the mark under the wrong word. */
void test_offsets_are_characters()
{
    const std::vector<wordsmith::Word> words =
        wordsmith::words_in("café au lait");

    check_equal(words.size(), std::size_t{ 3 }, "three words");
    if (words.size() < 3) {
        return;
    }

    check_equal(words[0].offset, std::size_t{ 0 }, "the first word starts at 0");
    check_equal(words[0].length, std::size_t{ 4 }, "café is four characters");
    check_equal(words[1].offset, std::size_t{ 5 },
                "the second starts one character past it, not two bytes further");
    check_equal(words[2].offset, std::size_t{ 8 }, "and the third follows");
}

void test_offsets_survive_trimming()
{
    const std::vector<wordsmith::Word> words = wordsmith::words_in("‘tis’");

    check_equal(words.size(), std::size_t{ 1 }, "one word inside the quotes");
    if (words.empty()) {
        return;
    }
    check_equal(words[0].offset, std::size_t{ 1 }, "starting after the quote");
    check_equal(words[0].length, std::size_t{ 3 }, "and ending before it");
}

/* Malformed bytes are stepped over as breaks rather than stopping the pass: a
 * buffer from GTK is always well-formed, and text from anywhere else has no
 * business leaving the rest of the line unchecked. */
void test_malformed_bytes_do_not_stop_the_pass()
{
    const std::string text = std::string("alpha \xFF\xFE beta");
    check_equal(words_of(text), "alpha beta", "the words either side are found");
}

/* ── the dictionary ─────────────────────────────────────────────────────── */

/* Nothing here can assume a dictionary is installed, so what is checked is the
 * contract that holds either way: with one, a misspelling is caught; without
 * one, everything is known and the editor marks nothing. */
void test_checker_never_marks_without_a_dictionary()
{
    const wordsmith::SpellChecker checker;

    check(checker.knows(""), "an empty word is never a mistake");

    if (!checker.available()) {
        check(checker.knows("qwertzuiop"),
              "with no dictionary installed, every word is known");
        check(checker.corrections_for("qwertzuiop").empty(),
              "and nothing is suggested");
        std::cerr << "note: no dictionary installed; "
                     "the spelling checks were skipped\n";
        return;
    }

    check(!checker.language().empty(), "an open dictionary names its language");
    check(checker.knows("editor"), "an ordinary word is known");
    check(!checker.knows("qwertzuiop"), "a misspelling is not");
}

/* A word accepted for this sitting is known from then on. This is the seam the
 * manuscript's own names will arrive through. */
void test_accepting_a_word()
{
    wordsmith::SpellChecker checker;
    if (!checker.available()) {
        return;
    }

    const std::string invented = "Thessaly-Vorn";
    check(!checker.knows(invented), "an invented name starts out unknown");

    checker.accept(invented);
    check(checker.knows(invented), "and is known once it has been accepted");
}

/* ── what the dictionary adds ────────────────────────────────────────────── */

/* A character the dictionary names holds a word together from the inside, the
 * way an apostrophe does — including one this file has never heard of, which is
 * the whole point of asking. */
void test_the_dictionary_can_add_a_word_character()
{
    check_equal(words_of("na·ive"), "na ive", "a middle dot breaks a word");
    check_equal(words_of("na·ive", "·"), "na·ive",
                "unless the dictionary says it holds one together");

    check_equal(words_of("·quiet·", "·"), "quiet",
                "and one at either end is still trimmed off");
}

/* Only ever a break becoming part of a word. Everything this file already has
 * an opinion about keeps it, and the two that overlap for English are the ones
 * that would cost the most. */
void test_the_rules_win_where_they_have_an_opinion()
{
    /* English hunspell answers exactly this, and neither half may take. */
    const std::string english = "0123456789’";

    check_equal(words_of("v2 in 1984", english), "in",
                "the digits it names are still digits");
    check_equal(words_of("don’t", english), "don’t",
                "the typographic apostrophe it names was already inside a word");

    /* Enchant's own header warns the list may be a guess, and this is where a
     * wrong one costs a red line under an ordinary compound. */
    check_equal(words_of("well-known", "-"), "well known",
                "a hyphen breaks a word however loudly it is named");
    check_equal(words_of("well-known", "0123456789’-"), "well known",
                "including at the end of the list, where enchant puts it");
}

/* Nothing installed means no list, and the rules alone are the answer — the
 * same trade every other call here makes. */
void test_no_list_is_the_same_as_no_dictionary()
{
    check_equal(words_of("don't stop", ""), words_of("don't stop"),
                "an empty list changes nothing");

    wordsmith::SpellChecker checker;
    if (!checker.available()) {
        check(checker.extra_word_chars().empty(),
              "and without a dictionary there is no list to have");
    }
}

} // namespace

int main()
{
    test_plain_prose();
    test_apostrophes();
    test_hyphens_break_words();
    test_dashes_and_punctuation_break_words();
    test_anything_with_a_digit_is_left_alone();
    test_letters_above_ascii();
    test_offsets_are_characters();
    test_offsets_survive_trimming();
    test_malformed_bytes_do_not_stop_the_pass();
    test_the_dictionary_can_add_a_word_character();
    test_the_rules_win_where_they_have_an_opinion();
    test_no_list_is_the_same_as_no_dictionary();
    test_checker_never_marks_without_a_dictionary();
    test_accepting_a_word();

    return failures == 0 ? 0 : 1;
}
