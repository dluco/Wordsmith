#include "spelling.hpp"

#include <enchant.h>

#include <cstdlib>

namespace wordsmith {

namespace {

/* ── reading UTF-8 ───────────────────────────────────────────────────────── */

struct Decoded {
    char32_t    code  = 0;
    std::size_t bytes = 1;
    bool        valid = false;
};

/* The one code point at `at`. Malformed bytes are stepped over one at a time
 * and reported invalid, which the caller reads as a word break: a buffer coming
 * from GTK is always well-formed, and text arriving from anywhere else has no
 * business stopping the check. */
Decoded decode(std::string_view text, std::size_t at)
{
    const unsigned char lead = static_cast<unsigned char>(text[at]);

    Decoded out;
    if (lead < 0x80) {
        return { lead, 1, true };
    }
    if (lead >= 0xF0) {
        out.bytes = 4;
        out.code  = lead & 0x07u;
    } else if (lead >= 0xE0) {
        out.bytes = 3;
        out.code  = lead & 0x0Fu;
    } else if (lead >= 0xC0) {
        out.bytes = 2;
        out.code  = lead & 0x1Fu;
    } else {
        return {};   // a continuation byte with no lead in front of it
    }

    if (at + out.bytes > text.size()) {
        return {};
    }
    for (std::size_t index = 1; index < out.bytes; ++index) {
        const unsigned char next = static_cast<unsigned char>(text[at + index]);
        if ((next & 0xC0u) != 0x80u) {
            return {};
        }
        out.code = (out.code << 6) | (next & 0x3Fu);
    }

    out.valid = true;
    return out;
}

/* ── what a character does to a word ─────────────────────────────────────── */

enum class Kind { Letter, Digit, Apostrophe, Break };

/* The header states these rules; this is the whole of them.
 *
 * Above ASCII the default is Letter, so every accented, Greek and Cyrillic
 * character holds a word together without a Unicode table in the build. What
 * has to be named is the punctuation that does not: the Latin-1 supplement's
 * spacing and symbols, and the General Punctuation block, which is where prose
 * keeps its dashes, its quotation marks and its ellipsis. */
Kind kind_of(char32_t code)
{
    if (code < 0x80) {
        if ((code >= U'a' && code <= U'z') || (code >= U'A' && code <= U'Z')) {
            return Kind::Letter;
        }
        if (code >= U'0' && code <= U'9') {
            return Kind::Digit;
        }
        return code == U'\'' ? Kind::Apostrophe : Kind::Break;
    }

    /* The typographic apostrophe, which sits inside the punctuation block below
     * and is the one exception to it: this is the apostrophe a manuscript
     * written anywhere else arrives with. */
    if (code == 0x2019) {
        return Kind::Apostrophe;
    }
    if (code <= 0x00BF) {                        // NBSP through ¿
        return Kind::Break;
    }
    if (code == 0x00D7 || code == 0x00F7) {      // × and ÷, the two symbols
        return Kind::Break;                      // among the Latin-1 letters
    }
    if (code >= 0x2000 && code <= 0x206F) {      // dashes, quotes, ellipsis
        return Kind::Break;
    }
    return Kind::Letter;
}

/* Whether the dictionary named this character as one that holds a word
 * together.
 *
 * The hyphen is refused however loudly it is named. Enchant's own header warns
 * that the list may be a guess, and this is the character where a wrong guess
 * costs the most: joining "well-known" into one token asks the dictionary about
 * a compound it almost certainly does not hold, and puts a red line under an
 * ordinary word. The halves are what a dictionary keeps, so the halves are what
 * it gets asked. */
bool named_by_dictionary(std::string_view extra_word_chars, char32_t code)
{
    if (code == U'-' || extra_word_chars.empty()) {
        return false;
    }

    std::size_t at = 0;
    while (at < extra_word_chars.size()) {
        const Decoded step = decode(extra_word_chars, at);
        if (step.valid && step.code == code) {
            return true;
        }
        at += step.bytes;
    }
    return false;
}

} // namespace

std::vector<Word> words_in(std::string_view text, std::string_view extra_word_chars)
{
    std::vector<Word> words;

    std::size_t byte      = 0;
    std::size_t character = 0;

    /* The token being gathered. It opens on a letter or a digit — never on an
     * apostrophe, which is how a leading quote is trimmed — and `core` follows
     * the last letter or digit in it, which is how a trailing one is. */
    bool        gathering       = false;
    bool        has_digit       = false;
    std::size_t start_byte      = 0;
    std::size_t start_character = 0;
    std::size_t core_byte       = 0;
    std::size_t core_character   = 0;

    const auto flush = [&]() {
        if (gathering && !has_digit && core_byte > start_byte) {
            Word word;
            word.offset = start_character;
            word.length = core_character - start_character;
            word.text.assign(text.substr(start_byte, core_byte - start_byte));
            words.push_back(std::move(word));
        }
        gathering = false;
        has_digit = false;
    };

    while (byte < text.size()) {
        const Decoded step = decode(text, byte);
        Kind          kind = step.valid ? kind_of(step.code) : Kind::Break;

        /* Only ever a break becoming an apostrophe: the rules above have the
         * first word, and the dictionary's list can add a character that holds
         * a word together but cannot take one away. */
        if (kind == Kind::Break && step.valid
            && named_by_dictionary(extra_word_chars, step.code)) {
            kind = Kind::Apostrophe;
        }

        switch (kind) {
        case Kind::Break:
            flush();
            break;

        case Kind::Apostrophe:
            /* Only ever inside a word. One that opens a token is quotation, and
             * the token simply has not started yet. */
            break;

        case Kind::Letter:
        case Kind::Digit:
            if (!gathering) {
                gathering       = true;
                start_byte      = byte;
                start_character = character;
            }
            if (kind == Kind::Digit) {
                has_digit = true;
            }
            core_byte      = byte + step.bytes;
            core_character = character + 1;
            break;
        }

        byte += step.bytes;
        character++;
    }
    flush();

    return words;
}

/* ── the dictionary ──────────────────────────────────────────────────────── */

struct SpellChecker::Dictionary {
    EnchantBroker* broker = nullptr;
    EnchantDict*   handle = nullptr;

    ~Dictionary()
    {
        if (handle != nullptr) {
            enchant_broker_free_dict(broker, handle);
        }
        if (broker != nullptr) {
            enchant_broker_free(broker);
        }
    }
};

namespace {

/* The language the environment asks for, named the way enchant wants it: the
 * first locale variable that says anything, with the codeset and any modifier
 * trimmed off. `C` and `POSIX` say nothing about a language and are skipped. */
std::string locale_language()
{
    for (const char* name : { "LC_ALL", "LC_MESSAGES", "LANG" }) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        std::string tag(value);
        tag = tag.substr(0, tag.find_first_of(".@"));
        if (tag.empty() || tag == "C" || tag == "POSIX") {
            continue;
        }
        return tag;
    }
    return {};
}

/* The language part of a locale: `en` out of `en_GB`. Empty when there is no
 * country to drop. */
std::string without_country(const std::string& tag)
{
    const std::size_t underscore = tag.find('_');
    return underscore == std::string::npos ? std::string() : tag.substr(0, underscore);
}

} // namespace

SpellChecker::SpellChecker() : SpellChecker(locale_language()) {}

SpellChecker::SpellChecker(const std::string& language)
    : dictionary_(std::make_unique<Dictionary>())
{
    dictionary_->broker = enchant_broker_init();
    if (dictionary_->broker == nullptr) {
        return;
    }

    /* Asked for first, then the language without its country, so an author with
     * `en_GB` set and only `en` installed is still checked. Nothing else is
     * tried: guessing at a language the author does not write in would mark
     * every word on the page. */
    for (const std::string& tag : { language, without_country(language) }) {
        if (tag.empty()) {
            continue;
        }
        dictionary_->handle = enchant_broker_request_dict(dictionary_->broker,
                                                          tag.c_str());
        if (dictionary_->handle != nullptr) {
            language_ = tag;
            /* Read once, here: it cannot change under an open dictionary, and
             * the caller asks for it on every line it checks. */
            const char* extra =
                enchant_dict_get_extra_word_characters(dictionary_->handle);
            if (extra != nullptr) {
                extra_word_chars_ = extra;
            }
            return;
        }
    }
}

SpellChecker::~SpellChecker() = default;

bool SpellChecker::available() const
{
    return dictionary_ != nullptr && dictionary_->handle != nullptr;
}

const std::string& SpellChecker::language() const { return language_; }

const std::string& SpellChecker::extra_word_chars() const
{
    return extra_word_chars_;
}

bool SpellChecker::knows(const std::string& word) const
{
    if (!available() || word.empty()) {
        return true;
    }
    /* Zero means the dictionary has the word; anything negative is a failure to
     * ask, which is not the author's mistake and is not marked as one. */
    return enchant_dict_check(dictionary_->handle, word.c_str(),
                              static_cast<ssize_t>(word.size()))
        <= 0;
}

std::vector<std::string> SpellChecker::corrections_for(const std::string& word) const
{
    std::vector<std::string> corrections;
    if (!available() || word.empty()) {
        return corrections;
    }

    std::size_t count = 0;
    char** suggestions = enchant_dict_suggest(dictionary_->handle, word.c_str(),
                                              static_cast<ssize_t>(word.size()),
                                              &count);
    if (suggestions == nullptr) {
        return corrections;
    }

    corrections.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        corrections.emplace_back(suggestions[index]);
    }
    enchant_dict_free_string_list(dictionary_->handle, suggestions);
    return corrections;
}

void SpellChecker::accept(const std::string& word)
{
    if (!available() || word.empty()) {
        return;
    }
    enchant_dict_add_to_session(dictionary_->handle, word.c_str(),
                                static_cast<ssize_t>(word.size()));
}

void SpellChecker::remember(const std::string& word)
{
    if (!available() || word.empty()) {
        return;
    }
    enchant_dict_add(dictionary_->handle, word.c_str(),
                     static_cast<ssize_t>(word.size()));
}

} // namespace wordsmith
