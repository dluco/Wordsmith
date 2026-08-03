#include "preferences.hpp"

#include "safe-write.hpp"

#include <argo/argo.hpp>
#include <argo/exceptions.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace wordsmith {

namespace {

/* Bumped when the on-disk shape changes in a way older builds cannot read.
 * Unlike the project file, a version we do not understand is not refused: see
 * the header on why nothing here is worth failing over. */
constexpr std::int64_t PREFERENCES_FORMAT_VERSION = 1;

constexpr const char* CONFIG_SUBDIRECTORY = "wordsmith";
constexpr const char* PREFERENCES_FILE_NAME = "settings.json";

constexpr const char* VERSION_KEY = "version";
constexpr const char* TEXT_SCALE_KEY = "editor-text-scale";
constexpr const char* SPELL_CHECK_KEY = "spell-check";
constexpr const char* AUTOFORMAT_LISTS_KEY = "autoformat-lists";

/* An environment variable's value, or empty when it is unset or blank. XDG
 * treats a set-but-empty variable as unset, and so does this. */
std::string environment(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

/* Whatever number `field` holds, as a text-scale percentage. JSON has one
 * number type but argo distinguishes how it was written, so `100`, `100.0` and
 * a value large enough to have been read as unsigned all have to arrive at the
 * same place.
 *
 * The clamp happens here, on the double, rather than on the result: a
 * hand-typed `999999999999` does not survive the conversion to `int`. */
bool read_text_scale(const argo::json& field, int& out)
{
    double value = 0.0;
    if (field.is_integer()) {
        value = static_cast<double>(field.get_integer());
    } else if (field.is_unsigned()) {
        value = static_cast<double>(field.get_unsigned());
    } else if (field.is_float()) {
        value = field.get_float();
    } else {
        return false;
    }

    out = static_cast<int>(std::clamp(value,
                                      static_cast<double>(TEXT_SCALE_MIN_PERCENT),
                                      static_cast<double>(TEXT_SCALE_MAX_PERCENT)));
    return true;
}

/* Every boolean here follows one rule, so it is written down once: only an
 * outright `false` overrules the default. Anything else — a number, a string, a
 * key that is not there at all — leaves `out` alone, which is what the header
 * means by every way of not saying anything. */
void read_flag(const std::map<std::string, argo::json>& fields, const char* key,
               bool& out)
{
    auto field = fields.find(key);
    if (field != fields.end() && field->second.is_boolean()) {
        out = field->second.get_bool();
    }
}

} // namespace

fs::path preferences_path()
{
    const std::string config_home = environment("XDG_CONFIG_HOME");
    if (!config_home.empty()) {
        return fs::path(config_home) / CONFIG_SUBDIRECTORY / PREFERENCES_FILE_NAME;
    }

    const std::string home = environment("HOME");
    const fs::path base = home.empty() ? fs::path(".") : fs::path(home) / ".config";
    return base / CONFIG_SUBDIRECTORY / PREFERENCES_FILE_NAME;
}

int clamp_text_scale(int percent)
{
    return std::clamp(percent, TEXT_SCALE_MIN_PERCENT, TEXT_SCALE_MAX_PERCENT);
}

Preferences load_preferences(const fs::path& file)
{
    Preferences preferences;

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return preferences;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    argo::json document;
    try {
        document = argo::json::parse(buffer.str());
    } catch (const argo::json_error&) {
        return preferences;
    }
    if (!document.is_object()) {
        return preferences;
    }

    const std::map<std::string, argo::json>& fields = document.get_object();
    auto text_scale = fields.find(TEXT_SCALE_KEY);
    int percent = 0;
    if (text_scale != fields.end() && read_text_scale(text_scale->second, percent)) {
        preferences.editor_text_scale_percent = clamp_text_scale(percent);
    }

    read_flag(fields, SPELL_CHECK_KEY, preferences.spell_check);
    read_flag(fields, AUTOFORMAT_LISTS_KEY, preferences.autoformat_lists);

    return preferences;
}

bool save_preferences(const Preferences& preferences, const fs::path& file,
                      std::string& error)
{
    const fs::path directory = file.parent_path();
    if (!directory.empty()) {
        std::error_code code;
        fs::create_directories(directory, code);
        if (code) {
            error = "cannot create " + directory.string() + ": " + code.message();
            return false;
        }
    }

    argo::json document = argo::json::object({
        {VERSION_KEY, argo::json::integer_value(PREFERENCES_FORMAT_VERSION)},
        {TEXT_SCALE_KEY,
         argo::json::integer_value(clamp_text_scale(preferences.editor_text_scale_percent))},
        {SPELL_CHECK_KEY, argo::json::boolean_value(preferences.spell_check)},
        {AUTOFORMAT_LISTS_KEY, argo::json::boolean_value(preferences.autoformat_lists)},
    });

    return write_file_atomically(file, document.serialize() + "\n", error);
}

} // namespace wordsmith
