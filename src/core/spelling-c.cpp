#include "spelling-c.h"

#include "spelling.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

/* malloc'd, so everything handed over this bridge is freed with free() — the
 * same rule wordsmith_free_string() states. */
char* duplicate(const std::string& text)
{
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

} // namespace

struct WordsmithSpellChecker {
    wordsmith::SpellChecker checker;
};

WordsmithSpellWord* wordsmith_spell_words(const char* text,
                                          const char* extra_word_chars,
                                          size_t* count)
{
    if (count != nullptr) {
        *count = 0;
    }
    if (text == nullptr) {
        return nullptr;
    }

    const std::vector<wordsmith::Word> found = wordsmith::words_in(
        text, extra_word_chars != nullptr ? extra_word_chars : "");
    if (found.empty()) {
        return nullptr;
    }

    WordsmithSpellWord* words = static_cast<WordsmithSpellWord*>(
        std::calloc(found.size(), sizeof(WordsmithSpellWord)));
    if (words == nullptr) {
        return nullptr;
    }

    for (std::size_t index = 0; index < found.size(); ++index) {
        words[index].offset = static_cast<int>(found[index].offset);
        words[index].length = static_cast<int>(found[index].length);
        words[index].text   = duplicate(found[index].text);
        if (words[index].text == nullptr) {
            /* Out of memory partway: hand back nothing rather than a half-built
             * array the caller would have to reason about. */
            wordsmith_spell_words_free(words, index);
            return nullptr;
        }
    }

    if (count != nullptr) {
        *count = found.size();
    }
    return words;
}

void wordsmith_spell_words_free(WordsmithSpellWord* words, size_t count)
{
    if (words == nullptr) {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        std::free(words[index].text);
    }
    std::free(words);
}

WordsmithSpellChecker* wordsmith_spell_checker_new(void)
{
    return new (std::nothrow) WordsmithSpellChecker();
}

void wordsmith_spell_checker_free(WordsmithSpellChecker* checker)
{
    delete checker;
}

int wordsmith_spell_checker_available(const WordsmithSpellChecker* checker)
{
    return checker != nullptr && checker->checker.available() ? 1 : 0;
}

const char* wordsmith_spell_checker_language(const WordsmithSpellChecker* checker)
{
    return checker != nullptr ? checker->checker.language().c_str() : "";
}

const char* wordsmith_spell_checker_extra_word_chars(
    const WordsmithSpellChecker* checker)
{
    return checker != nullptr ? checker->checker.extra_word_chars().c_str() : "";
}

int wordsmith_spell_checker_knows(const WordsmithSpellChecker* checker,
                                  const char* word)
{
    if (checker == nullptr || word == nullptr) {
        return 1;
    }
    return checker->checker.knows(word) ? 1 : 0;
}

char** wordsmith_spell_checker_corrections(const WordsmithSpellChecker* checker,
                                           const char* word, size_t* count)
{
    if (count != nullptr) {
        *count = 0;
    }
    if (checker == nullptr || word == nullptr) {
        return nullptr;
    }

    const std::vector<std::string> found = checker->checker.corrections_for(word);
    if (found.empty()) {
        return nullptr;
    }

    char** corrections =
        static_cast<char**>(std::calloc(found.size(), sizeof(char*)));
    if (corrections == nullptr) {
        return nullptr;
    }

    for (std::size_t index = 0; index < found.size(); ++index) {
        corrections[index] = duplicate(found[index]);
        if (corrections[index] == nullptr) {
            wordsmith_spell_corrections_free(corrections, index);
            return nullptr;
        }
    }

    if (count != nullptr) {
        *count = found.size();
    }
    return corrections;
}

void wordsmith_spell_corrections_free(char** corrections, size_t count)
{
    if (corrections == nullptr) {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        std::free(corrections[index]);
    }
    std::free(corrections);
}

void wordsmith_spell_checker_accept(WordsmithSpellChecker* checker,
                                    const char* word)
{
    if (checker != nullptr && word != nullptr) {
        checker->checker.accept(word);
    }
}

void wordsmith_spell_checker_remember(WordsmithSpellChecker* checker,
                                      const char* word)
{
    if (checker != nullptr && word != nullptr) {
        checker->checker.remember(word);
    }
}
