//
// Finding the words in a stretch of prose, and asking whether each one is
// spelled the way the language spells it.
//
// Two halves that are deliberately separable. `words_in()` is pure text
// handling and has no dictionary behind it; `SpellChecker` is the dictionary
// and knows nothing about where a word came from. The editor needs both, but
// only the first can be checked without a dictionary installed, and it is the
// half that holds the rules worth arguing about.
//
// C++17, no GLib. Enchant is a plain C API over whatever dictionaries the
// system has — hunspell in practice — and its header pulls in nothing but
// <stdint.h>, so it does not cost this layer its independence. It is here
// rather than in the UI because *what counts as a misspelling* is a question
// about the manuscript, not about how it is drawn; the UI's only job is the red
// line under the answer.
//
// ## Offsets are characters
//
// Every offset and length below counts characters, not bytes, because the only
// caller places GtkTextIters with them and that is the unit GTK counts in. The
// same rule undo-stack.h states, for the same reason: byte offsets work until
// the first accented character and then put the mark in the wrong place.
//

#ifndef WORDSMITH_SPELLING_HPP
#define WORDSMITH_SPELLING_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wordsmith {

/** One word found in a stretch of text: where it is, and what it says. */
struct Word {
    std::size_t offset = 0;   // characters from the start of the text
    std::size_t length = 0;   // characters
    std::string text;
};

/** Every word in `text` worth putting to a dictionary, in order.
 *
 *  What counts as a word is a judgement about prose, and these are the rules:
 *
 *    - Letters make a word. Anything above ASCII is a letter unless it is
 *      punctuation prose actually uses — the dashes, quotes and ellipsis of the
 *      General Punctuation block, and the symbols of the Latin-1 supplement.
 *      That way é and ü hold a word together and an em dash breaks one, without
 *      this file carrying a copy of the Unicode tables.
 *    - An apostrophe inside a word belongs to it, so "don't" is one word and
 *      not two. Both the typed ' and the typographic ’ count, because a
 *      manuscript pasted in from elsewhere is full of the second. One at either
 *      end is quotation and is trimmed off.
 *    - A hyphen breaks a word. Dictionaries hold "well" and "known" but rarely
 *      "well-known", so checking the halves separately is what avoids marking
 *      an ordinary compound as wrong.
 *    - Anything with a digit in it is not offered at all: "1984", "3rd" and
 *      "v2" are not spelling mistakes and no dictionary has an opinion on them.
 *
 *  `extra_word_chars` is the dictionary's own answer to the same question —
 *  SpellChecker::extra_word_chars(), which for English is `0123456789’`. Every
 *  character in it holds a word together from the inside, exactly as an
 *  apostrophe does, so a language whose words are held together by something
 *  this file never thought of is read correctly without this file learning it.
 *
 *  **The rules above win where they have an opinion.** The dictionary's list is
 *  advisory — enchant says outright that for some back-ends it is a guess — and
 *  the two places it overlaps are already answered here: the digits it names
 *  are still digits, and the hyphen it may name still breaks a word, because a
 *  guess that joins "well-known" costs a false mark under every ordinary
 *  compound. What the list can do is add a character that would otherwise have
 *  broken a word; it cannot take one back.
 *
 *  This keeps its own contract in the bargain: the parameter is a string, not a
 *  dictionary, so every rule here is still checkable on a machine with nothing
 *  installed.
 *
 *  A bare URL typed into the manuscript is checked a word at a time, and its
 *  host will be marked. That is left alone on purpose: Markdown links keep
 *  their target out of the editor's buffer entirely, so the case only arises
 *  for a URL an author typed as prose, and the rules above are worth more than
 *  a special case for it. */
std::vector<Word> words_in(std::string_view text,
                           std::string_view extra_word_chars = {});

/**
 * The system's dictionary for one language.
 *
 * Every failure here is silence rather than an error, the same trade
 * preferences.hpp makes: an author with no dictionary installed gets an editor
 * that does not mark anything, which is exactly the editor they had before this
 * existed. Refusing to open a manuscript over a missing word list would be
 * absurd, so `available()` is offered for anyone who wants to say so quietly
 * and `knows()` answers true for everything when there is nothing to ask.
 */
class SpellChecker {
public:
    /** The dictionary for the system's language, from the environment's locale.
     *  `LC_ALL`, `LC_MESSAGES` and `LANG` in that order, with the codeset and
     *  modifier trimmed off: `en_GB.UTF-8` asks for `en_GB`, and falls back to
     *  plain `en` when the country has no dictionary of its own. */
    SpellChecker();

    /** The dictionary for `language`, named the way a locale is: `en_US`, `fr`.
     *  Nothing chooses one yet — the seam is here so a preference can, without
     *  the rest of this file changing. */
    explicit SpellChecker(const std::string& language);

    ~SpellChecker();

    SpellChecker(const SpellChecker&) = delete;
    SpellChecker& operator=(const SpellChecker&) = delete;

    /** Whether a dictionary was found. False makes every other call inert. */
    bool available() const;

    /** The language that was actually opened, which need not be the one asked
     *  for: `en_GB` may have been answered by `en`. Empty when none was. */
    const std::string& language() const;

    /** The non-letters this dictionary allows inside a word — `0123456789’` for
     *  English, and a hyphen last when there is one. Empty when there is no
     *  dictionary to ask.
     *
     *  It is here to be handed to words_in(), which is the only thing that
     *  wants it: the dictionary knows what holds a word together in its own
     *  language, and hard-coding that here would be this file guessing at
     *  languages nobody has written it for. Read once when the dictionary is
     *  opened, since it cannot change under an open one. */
    const std::string& extra_word_chars() const;

    /** Whether `word` is spelled the way the dictionary spells it. True — no
     *  mark — whenever there is no dictionary to ask, or the word is empty. */
    bool knows(const std::string& word) const;

    /** What `word` might have been meant to be, best first. Empty when there is
     *  nothing to ask or nothing to suggest. */
    std::vector<std::string> corrections_for(const std::string& word) const;

    /** Accept `word` for this sitting only, writing nothing to disk.
     *
     *  This is the seam the manuscript's own vocabulary arrives through. A
     *  novel is full of names and invented terms that are spelled right and
     *  are in no dictionary, and the answer is to hand them to the checker when
     *  the project opens rather than to ask the author to teach the machine
     *  their character list one squiggle at a time. Nothing here reads the
     *  manuscript yet; when it does, this is the call it will make. */
    void accept(const std::string& word);

    /** Accept `word` for good, in the personal word list the dictionary keeps
     *  for this user. For "Add to Dictionary", where the author has said this
     *  is a word and means it beyond this project. */
    void remember(const std::string& word);

private:
    /* Enchant's broker and dictionary, out of line so no caller of this header
     * has to have enchant's. */
    struct Dictionary;
    std::unique_ptr<Dictionary> dictionary_;
    std::string                 language_;
    std::string                 extra_word_chars_;
};

} // namespace wordsmith

#endif /* WORDSMITH_SPELLING_HPP */
