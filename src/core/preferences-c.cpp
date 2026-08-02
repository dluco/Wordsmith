#include "preferences-c.h"

#include "preferences.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

/* The C header states the bounds as macros so the UI can build a menu around
 * them without a call; they have to be the same numbers the core clamps to. */
static_assert(WORDSMITH_TEXT_SCALE_MIN_PERCENT == wordsmith::TEXT_SCALE_MIN_PERCENT,
              "text scale bounds disagree across the bridge");
static_assert(WORDSMITH_TEXT_SCALE_MAX_PERCENT == wordsmith::TEXT_SCALE_MAX_PERCENT,
              "text scale bounds disagree across the bridge");
static_assert(WORDSMITH_TEXT_SCALE_DEFAULT_PERCENT
                  == wordsmith::TEXT_SCALE_DEFAULT_PERCENT,
              "text scale default disagrees across the bridge");
static_assert((WORDSMITH_SPELL_CHECK_DEFAULT != 0) == wordsmith::SPELL_CHECK_DEFAULT,
              "spell check default disagrees across the bridge");

namespace {

char* duplicate(const std::string& text)
{
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

void set_error(char** error, const std::string& message)
{
    if (error != nullptr) {
        *error = duplicate(message);
    }
}

} // namespace

int wordsmith_preferences_text_scale(void)
{
    return wordsmith::load_preferences(wordsmith::preferences_path())
        .editor_text_scale_percent;
}

int wordsmith_preferences_set_text_scale(int percent, char** error)
{
    /* Read before writing, rather than writing a struct built from nothing:
     * saving re-emits every field, so anything else in the file has to be
     * carried across. */
    const std::filesystem::path file = wordsmith::preferences_path();
    wordsmith::Preferences preferences = wordsmith::load_preferences(file);
    preferences.editor_text_scale_percent = wordsmith::clamp_text_scale(percent);

    std::string message;
    if (!wordsmith::save_preferences(preferences, file, message)) {
        set_error(error, message);
        return 0;
    }
    return 1;
}

int wordsmith_preferences_spell_check(void)
{
    return wordsmith::load_preferences(wordsmith::preferences_path()).spell_check ? 1
                                                                                  : 0;
}

int wordsmith_preferences_set_spell_check(int enabled, char** error)
{
    /* Read first, for the reason above: the size in the file is not this
     * caller's to drop. */
    const std::filesystem::path file = wordsmith::preferences_path();
    wordsmith::Preferences preferences = wordsmith::load_preferences(file);
    preferences.spell_check = enabled != 0;

    std::string message;
    if (!wordsmith::save_preferences(preferences, file, message)) {
        set_error(error, message);
        return 0;
    }
    return 1;
}
