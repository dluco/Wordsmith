#include "core/preferences.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        failures++;
    }
}

void check_equal(int actual, int expected, const std::string& what)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << what << "\n  expected: " << expected
                  << "\n  actual:   " << actual << '\n';
        failures++;
    }
}

/* A scratch directory removed on destruction, so a failing check does not
 * leave litter behind. */
class TempDir {
public:
    TempDir()
    {
        std::error_code code;
        root_ = fs::temp_directory_path(code)
            / ("wordsmith-preferences-" + std::to_string(::getpid()) + "-"
               + std::to_string(counter_++));
        fs::create_directories(root_, code);
    }

    ~TempDir()
    {
        std::error_code code;
        fs::remove_all(root_, code);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return root_; }

private:
    fs::path          root_;
    static inline int counter_ = 0;
};

void write_file(const fs::path& path, const std::string& contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
}

std::string read_file(const fs::path& path)
{
    std::ifstream      stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/* ── tests ──────────────────────────────────────────────────────────────── */

void test_missing_file_gives_defaults()
{
    TempDir temp;
    const wordsmith::Preferences preferences =
        wordsmith::load_preferences(temp.path() / "nothing-here.json");

    check_equal(preferences.editor_text_scale_percent,
                wordsmith::TEXT_SCALE_DEFAULT_PERCENT,
                "a missing preferences file reads as defaults");
    check(preferences.spell_check,
          "and spelling is checked until the author says otherwise");
    check(preferences.autoformat_lists,
          "and a typed marker makes a list until they say otherwise");
}

void test_round_trip()
{
    TempDir temp;
    const fs::path file = temp.path() / "settings.json";

    wordsmith::Preferences written;
    written.editor_text_scale_percent = 140;
    written.spell_check               = false;
    written.autoformat_lists          = false;

    std::string error;
    check(wordsmith::save_preferences(written, file, error),
          "saving preferences succeeds: " + error);

    const wordsmith::Preferences read = wordsmith::load_preferences(file);
    check_equal(read.editor_text_scale_percent, 140, "the text size round-trips");
    check(!read.spell_check, "and so does spelling turned off");
    check(!read.autoformat_lists, "and so do automatic lists turned off");
}

/* Off is the only answer that has to survive being written down, since on is
 * what every other reading gives. It shares the file with the text size, so
 * setting either has to leave the other alone. */
void test_spell_check_is_read_and_written_beside_the_text_size()
{
    TempDir temp;

    const fs::path off = temp.path() / "off.json";
    write_file(off, "{\"editor-text-scale\": 110, \"spell-check\": false}\n");
    const wordsmith::Preferences read = wordsmith::load_preferences(off);
    check(!read.spell_check, "an explicit false turns the marking off");
    check_equal(read.editor_text_scale_percent, 110,
                "and leaves the text size where it was");

    const fs::path on = temp.path() / "on.json";
    write_file(on, "{\"spell-check\": true}\n");
    check(wordsmith::load_preferences(on).spell_check, "an explicit true turns it on");

    const fs::path file = temp.path() / "settings.json";
    std::string    error;
    wordsmith::save_preferences(wordsmith::Preferences{}, file, error);
    check(read_file(file).find("\"spell-check\"") != std::string::npos,
          "the answer is written under a named key");
}

/* Every way of not saying anything means on: no key, the wrong type, a file
 * that is not JSON at all. A missing answer must not leave a manuscript
 * quietly unchecked. */
void test_only_an_outright_false_turns_spell_check_off()
{
    TempDir temp;

    const fs::path no_key = temp.path() / "no-key.json";
    write_file(no_key, "{\"editor-text-scale\": 100}\n");
    check(wordsmith::load_preferences(no_key).spell_check,
          "a file without the key reads as on");

    const fs::path wrong_type = temp.path() / "string.json";
    write_file(wrong_type, "{\"spell-check\": \"no\"}\n");
    check(wordsmith::load_preferences(wrong_type).spell_check,
          "a hand-edited string reads as on");

    const fs::path zero = temp.path() / "zero.json";
    write_file(zero, "{\"spell-check\": 0}\n");
    check(wordsmith::load_preferences(zero).spell_check,
          "and so does a number, however plainly it was meant");

    const fs::path malformed = temp.path() / "malformed.json";
    write_file(malformed, "{\"spell-check\": fal");
    check(wordsmith::load_preferences(malformed).spell_check,
          "and so does a truncated file");
}

/* The second boolean, and the first thing in this file that could be confused
 * with something else in it: two flags of the same type are a swap nothing
 * downstream would catch — the author would just find the wrong thing turned
 * off — so each is read with the other set the opposite way. */
void test_automatic_lists_is_its_own_answer()
{
    TempDir temp;

    const fs::path mixed = temp.path() / "mixed.json";
    write_file(mixed, "{\"spell-check\": true, \"autoformat-lists\": false}\n");
    const wordsmith::Preferences read = wordsmith::load_preferences(mixed);
    check(read.spell_check, "spelling is read from its own key");
    check(!read.autoformat_lists, "and automatic lists from theirs");

    const fs::path swapped = temp.path() / "swapped.json";
    write_file(swapped, "{\"spell-check\": false, \"autoformat-lists\": true}\n");
    const wordsmith::Preferences other = wordsmith::load_preferences(swapped);
    check(!other.spell_check, "the other way round as well");
    check(other.autoformat_lists, "for both of them");

    /* And the rule every boolean here inherits, which this one is the second to
     * inherit rather than the first to state. */
    const fs::path no_key = temp.path() / "no-lists-key.json";
    write_file(no_key, "{\"spell-check\": false}\n");
    check(wordsmith::load_preferences(no_key).autoformat_lists,
          "a file without the key reads as on");

    const fs::path wrong_type = temp.path() / "lists-string.json";
    write_file(wrong_type, "{\"autoformat-lists\": \"no\"}\n");
    check(wordsmith::load_preferences(wrong_type).autoformat_lists,
          "and so does a hand-edited string");

    const fs::path file = temp.path() / "settings.json";
    std::string    error;
    wordsmith::save_preferences(wordsmith::Preferences{}, file, error);
    check(read_file(file).find("\"autoformat-lists\"") != std::string::npos,
          "the answer is written under a named key");
}

/* The file is meant to be legible to whoever opens it, and editable in place. */
void test_saved_file_is_json_an_author_can_edit()
{
    TempDir temp;
    const fs::path file = temp.path() / "settings.json";

    std::string error;
    wordsmith::save_preferences(wordsmith::Preferences{}, file, error);

    const std::string contents = read_file(file);
    check(contents.find("\"editor-text-scale\"") != std::string::npos,
          "the text size is written under a named key");
    check(contents.find("\"version\"") != std::string::npos,
          "the format version is written");
    check(!contents.empty() && contents.back() == '\n',
          "the file ends with a newline");
}

/* The directory is the app's own, and a first run is the normal case. */
void test_save_creates_the_config_directory()
{
    TempDir temp;
    const fs::path file = temp.path() / "config" / "wordsmith" / "settings.json";

    std::string error;
    check(wordsmith::save_preferences(wordsmith::Preferences{}, file, error),
          "saving creates the directories it needs: " + error);
    check(fs::exists(file), "the preferences file was created");
}

void test_bad_files_read_as_defaults()
{
    TempDir temp;

    const fs::path malformed = temp.path() / "malformed.json";
    write_file(malformed, "{\"editor-text-scale\": ");
    check_equal(wordsmith::load_preferences(malformed).editor_text_scale_percent,
                wordsmith::TEXT_SCALE_DEFAULT_PERCENT,
                "truncated JSON reads as defaults");

    const fs::path not_an_object = temp.path() / "array.json";
    write_file(not_an_object, "[1, 2, 3]\n");
    check_equal(wordsmith::load_preferences(not_an_object).editor_text_scale_percent,
                wordsmith::TEXT_SCALE_DEFAULT_PERCENT,
                "a JSON array reads as defaults");

    const fs::path wrong_type = temp.path() / "string.json";
    write_file(wrong_type, "{\"editor-text-scale\": \"large\"}\n");
    check_equal(wordsmith::load_preferences(wrong_type).editor_text_scale_percent,
                wordsmith::TEXT_SCALE_DEFAULT_PERCENT,
                "a text size that is not a number reads as the default");
}

/* A hand-edited size is brought into range rather than refused: the point is a
 * readable window, not a correct file. */
void test_out_of_range_values_are_clamped()
{
    TempDir temp;

    const fs::path enormous = temp.path() / "enormous.json";
    write_file(enormous, "{\"editor-text-scale\": 999999999999}\n");
    check_equal(wordsmith::load_preferences(enormous).editor_text_scale_percent,
                wordsmith::TEXT_SCALE_MAX_PERCENT,
                "a size past the maximum is clamped down");

    const fs::path tiny = temp.path() / "tiny.json";
    write_file(tiny, "{\"editor-text-scale\": -40}\n");
    check_equal(wordsmith::load_preferences(tiny).editor_text_scale_percent,
                wordsmith::TEXT_SCALE_MIN_PERCENT,
                "a size below the minimum is clamped up");

    const fs::path fractional = temp.path() / "fractional.json";
    write_file(fractional, "{\"editor-text-scale\": 125.0}\n");
    check_equal(wordsmith::load_preferences(fractional).editor_text_scale_percent, 125,
                "a size written as a decimal is read");

    check_equal(wordsmith::clamp_text_scale(wordsmith::TEXT_SCALE_MAX_PERCENT + 1),
                wordsmith::TEXT_SCALE_MAX_PERCENT, "clamp_text_scale caps at the top");
    check_equal(wordsmith::clamp_text_scale(wordsmith::TEXT_SCALE_MIN_PERCENT - 1),
                wordsmith::TEXT_SCALE_MIN_PERCENT, "clamp_text_scale caps at the bottom");
    check_equal(wordsmith::clamp_text_scale(120), 120,
                "clamp_text_scale leaves a size in range alone");
}

/* Saving clamps too, so a caller that skipped the clamp cannot write a file
 * that would only be repaired on the way back in. */
void test_saving_clamps()
{
    TempDir temp;
    const fs::path file = temp.path() / "settings.json";

    wordsmith::Preferences preferences;
    preferences.editor_text_scale_percent = 5000;

    std::string error;
    wordsmith::save_preferences(preferences, file, error);

    check_equal(wordsmith::load_preferences(file).editor_text_scale_percent,
                wordsmith::TEXT_SCALE_MAX_PERCENT, "an out-of-range size is saved clamped");
}

/* The location follows XDG: the variable when it is set, ~/.config otherwise. */
void test_path_follows_xdg()
{
    /* Copied out before anything is set: setenv may move what the environment
     * pointers point at. */
    const char* config_home_value = std::getenv("XDG_CONFIG_HOME");
    const char* home_value = std::getenv("HOME");
    const bool  had_config_home = config_home_value != nullptr;
    const bool  had_home = home_value != nullptr;
    const std::string saved_config_home = had_config_home ? config_home_value : "";
    const std::string saved_home = had_home ? home_value : "";

    ::setenv("XDG_CONFIG_HOME", "/somewhere/config", 1);
    check(wordsmith::preferences_path()
              == fs::path("/somewhere/config/wordsmith/settings.json"),
          "XDG_CONFIG_HOME decides where the file lives");

    ::unsetenv("XDG_CONFIG_HOME");
    ::setenv("HOME", "/home/author", 1);
    check(wordsmith::preferences_path()
              == fs::path("/home/author/.config/wordsmith/settings.json"),
          "without XDG_CONFIG_HOME the file lands under ~/.config");

    /* An empty variable counts as unset, the way XDG says it does. */
    ::setenv("XDG_CONFIG_HOME", "", 1);
    check(wordsmith::preferences_path()
              == fs::path("/home/author/.config/wordsmith/settings.json"),
          "an empty XDG_CONFIG_HOME falls back to ~/.config");

    if (had_config_home) {
        ::setenv("XDG_CONFIG_HOME", saved_config_home.c_str(), 1);
    } else {
        ::unsetenv("XDG_CONFIG_HOME");
    }
    if (had_home) {
        ::setenv("HOME", saved_home.c_str(), 1);
    } else {
        ::unsetenv("HOME");
    }
}

} // namespace

int main()
{
    test_missing_file_gives_defaults();
    test_round_trip();
    test_saved_file_is_json_an_author_can_edit();
    test_save_creates_the_config_directory();
    test_bad_files_read_as_defaults();
    test_out_of_range_values_are_clamped();
    test_saving_clamps();
    test_spell_check_is_read_and_written_beside_the_text_size();
    test_only_an_outright_false_turns_spell_check_off();
    test_automatic_lists_is_its_own_answer();
    test_path_follows_xdg();

    return failures == 0 ? 0 : 1;
}
