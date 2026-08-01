#include "core/session.hpp"

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

/* A scratch directory removed on destruction, so a failing check does not leave
 * litter behind. */
class TempDir {
public:
    TempDir()
    {
        std::error_code code;
        root_ = fs::temp_directory_path(code)
            / ("wordsmith-session-" + std::to_string(::getpid()) + "-"
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

/* A project root with a manuscript folder, one chapter folder and one document
 * in it, so existence checks have something real to answer about. */
fs::path make_project(const TempDir& temp, const std::string& name)
{
    const fs::path root = temp.path() / name;
    std::error_code code;
    fs::create_directories(root / "manuscript" / "act-one", code);
    write_file(root / "manuscript" / "act-one" / "scene.md", "# Scene\n");
    return root;
}

/* ── tests ──────────────────────────────────────────────────────────────── */

void test_missing_file_gives_nothing()
{
    TempDir temp;
    const fs::path root = make_project(temp, "novel");

    const wordsmith::ProjectSession session =
        wordsmith::session_for(temp.path() / "nothing-here.json", root);

    check(session.open_document.empty(),
          "a missing session file remembers no open document");
    check_equal(session.expanded.size(), 0u,
                "a missing session file remembers no expanded folders");
    check_equal(session.root, wordsmith::session_key(root).string(),
                "the returned session still names the project asked about");
    check_equal(wordsmith::load_session(temp.path() / "nothing-here.json").size(), 0u,
                "a missing session file lists no projects");
}

void test_round_trip()
{
    TempDir temp;
    const fs::path root = make_project(temp, "novel");
    const fs::path file = temp.path() / "session.json";

    wordsmith::ProjectSession written;
    written.root          = root.string();
    written.open_document = "manuscript/act-one/scene.md";
    written.expanded      = {"manuscript", "manuscript/act-one"};

    std::string error;
    check(wordsmith::save_project_session(written, file, error),
          "saving a session succeeds: " + error);

    const wordsmith::ProjectSession read = wordsmith::session_for(file, root);
    check_equal(read.open_document, "manuscript/act-one/scene.md",
                "the open document round-trips");
    check_equal(read.expanded.size(), 2u, "both expanded folders round-trip");
    if (read.expanded.size() == 2) {
        check_equal(read.expanded[0], "manuscript", "the first folder round-trips");
        check_equal(read.expanded[1], "manuscript/act-one",
                    "the second folder round-trips");
    }
}

/* The two pane flags are the remembered things that are not paths, so they have
 * no filesystem to be checked against and every way of not saying one has to
 * mean the same thing: the pane is on screen. */
void test_the_panes_are_remembered()
{
    TempDir temp;
    const fs::path root = make_project(temp, "novel");
    const fs::path file = temp.path() / "session.json";

    check(wordsmith::ProjectSession().binder_visible,
          "a session starts with the binder on screen");
    check(wordsmith::ProjectSession().inspector_visible,
          "a session starts with the inspector on screen");
    check(wordsmith::session_for(temp.path() / "nothing-here.json", root)
              .binder_visible,
          "a project nothing was saved for shows the binder");
    check(wordsmith::session_for(temp.path() / "nothing-here.json", root)
              .inspector_visible,
          "a project nothing was saved for shows the inspector");

    wordsmith::ProjectSession written;
    written.root              = root.string();
    written.binder_visible    = false;
    written.inspector_visible = false;

    std::string error;
    check(wordsmith::save_project_session(written, file, error),
          "saving two dismissed panes succeeds: " + error);
    check(!wordsmith::session_for(file, root).binder_visible,
          "a dismissed binder round-trips");
    check(!wordsmith::session_for(file, root).inspector_visible,
          "a dismissed inspector round-trips");

    /* One of each, which is what catches the two being written or read in the
     * wrong order — the failure a struct at the C bridge is there to prevent. */
    written.binder_visible    = false;
    written.inspector_visible = true;
    check(wordsmith::save_project_session(written, file, error),
          "saving one of each succeeds: " + error);
    check(!wordsmith::session_for(file, root).binder_visible,
          "the dismissed binder is the one that stays dismissed");
    check(wordsmith::session_for(file, root).inspector_visible,
          "the shown inspector is the one that stays shown");

    written.binder_visible    = true;
    written.inspector_visible = false;
    check(wordsmith::save_project_session(written, file, error),
          "saving the other way round succeeds: " + error);
    check(wordsmith::session_for(file, root).binder_visible,
          "and the flags do not trade places");
    check(!wordsmith::session_for(file, root).inspector_visible,
          "in either direction");

    /* An entry written before the fields existed, and one whose values someone
     * hand-edited into the wrong type. Neither may put a pane away. */
    const fs::path older = temp.path() / "older.json";
    write_file(older, "{\"projects\": [{\"root\": \"" + root.string() + "\"}]}\n");
    check(wordsmith::session_for(older, root).binder_visible,
          "an entry without the fields shows the binder");
    check(wordsmith::session_for(older, root).inspector_visible,
          "an entry without the fields shows the inspector");

    const fs::path wrong_type = temp.path() / "wrong-type.json";
    write_file(wrong_type, "{\"projects\": [{\"root\": \"" + root.string() +
                               "\", \"binder-visible\": 0, "
                               "\"inspector-visible\": \"no\"}]}\n");
    check(wordsmith::session_for(wrong_type, root).binder_visible,
          "a number reads as the default rather than as false");
    check(wordsmith::session_for(wrong_type, root).inspector_visible,
          "a string reads as the default rather than as false");
}

/* Two projects are two entries, and neither disturbs the other. */
void test_projects_are_kept_apart()
{
    TempDir temp;
    const fs::path novel = make_project(temp, "novel");
    const fs::path memoir = make_project(temp, "memoir");
    const fs::path file = temp.path() / "session.json";

    wordsmith::ProjectSession first;
    first.root          = novel.string();
    first.open_document = "manuscript/act-one/scene.md";

    wordsmith::ProjectSession second;
    second.root     = memoir.string();
    second.expanded = {"manuscript"};

    std::string error;
    wordsmith::save_project_session(first, file, error);
    wordsmith::save_project_session(second, file, error);

    check_equal(wordsmith::load_session(file).size(), 2u, "both projects are kept");
    check_equal(wordsmith::session_for(file, novel).open_document,
                "manuscript/act-one/scene.md",
                "the first project keeps its open document");
    check_equal(wordsmith::session_for(file, memoir).expanded.size(), 1u,
                "the second project keeps its expanded folder");
    check(wordsmith::session_for(file, memoir).open_document.empty(),
          "the second project did not inherit the first's open document");
}

/* Saving the same project again replaces its entry rather than adding one, and
 * moves it to the front. */
void test_saving_again_supersedes_and_promotes()
{
    TempDir temp;
    const fs::path novel = make_project(temp, "novel");
    const fs::path memoir = make_project(temp, "memoir");
    const fs::path file = temp.path() / "session.json";

    wordsmith::ProjectSession first;
    first.root     = novel.string();
    first.expanded = {"manuscript"};

    wordsmith::ProjectSession second;
    second.root = memoir.string();

    std::string error;
    wordsmith::save_project_session(first, file, error);
    wordsmith::save_project_session(second, file, error);

    first.expanded = {"manuscript", "manuscript/act-one"};
    wordsmith::save_project_session(first, file, error);

    const std::vector<wordsmith::ProjectSession> saved = wordsmith::load_session(file);
    check_equal(saved.size(), 2u, "re-saving a project does not add a second entry");
    if (!saved.empty()) {
        check_equal(saved.front().root, wordsmith::session_key(novel).string(),
                    "the project just saved is at the front");
        check_equal(saved.front().expanded.size(), 2u,
                    "the entry holds the newer expanded list");
    }
}

/* The same project reached by a different spelling is the same project. */
void test_the_key_is_normalised()
{
    TempDir temp;
    const fs::path novel = make_project(temp, "novel");
    const fs::path file = temp.path() / "session.json";

    wordsmith::ProjectSession written;
    written.root     = novel.string();
    written.expanded = {"manuscript"};

    std::string error;
    wordsmith::save_project_session(written, file, error);

    const fs::path roundabout = temp.path() / "novel" / "manuscript" / ".." ;
    check_equal(wordsmith::session_for(file, roundabout).expanded.size(), 1u,
                "a root spelled with .. finds the same entry");

    const fs::path trailing = fs::path(novel.string() + "/");
    check_equal(wordsmith::session_for(file, trailing).expanded.size(), 1u,
                "a root spelled with a trailing separator finds the same entry");

    written.root = trailing.string();
    wordsmith::save_project_session(written, file, error);
    check_equal(wordsmith::load_session(file).size(), 1u,
                "saving under a different spelling does not add a second entry");
}

/* What the filesystem no longer has cannot be restored, however it got that
 * way. Same contract as the `children:` list in a folder's sidecar. */
void test_stale_paths_are_dropped()
{
    TempDir temp;
    const fs::path root = make_project(temp, "novel");
    const fs::path file = temp.path() / "session.json";

    wordsmith::ProjectSession written;
    written.root          = root.string();
    written.open_document = "manuscript/act-one/gone.md";
    written.expanded      = {"manuscript", "manuscript/act-two", "manuscript/act-one"};

    std::string error;
    wordsmith::save_project_session(written, file, error);

    const wordsmith::ProjectSession read = wordsmith::session_for(file, root);
    check(read.open_document.empty(), "a deleted document is not restored");
    check_equal(read.expanded.size(), 2u, "a folder that is gone is not restored");
    if (read.expanded.size() == 2) {
        check_equal(read.expanded[1], "manuscript/act-one",
                    "the folders that remain keep their order");
    }

    /* A document that has become a folder is no more openable than one that was
     * deleted, and the reverse for an expanded folder. */
    std::error_code code;
    fs::create_directories(root / "manuscript" / "act-one" / "notes.md", code);
    write_file(root / "manuscript" / "act-two", "not a folder\n");

    wordsmith::ProjectSession confused;
    confused.root          = root.string();
    confused.open_document = "manuscript/act-one/notes.md";
    confused.expanded      = {"manuscript/act-two"};
    wordsmith::save_project_session(confused, file, error);

    const wordsmith::ProjectSession settled = wordsmith::session_for(file, root);
    check(settled.open_document.empty(), "a document that is now a folder is not opened");
    check_equal(settled.expanded.size(), 0u, "a folder that is now a file is not expanded");
}

/* The file is one an author may open, and the entries are legible without
 * knowing where the project lives. */
void test_saved_file_is_json_an_author_can_read()
{
    TempDir temp;
    const fs::path root = make_project(temp, "novel");
    const fs::path file = temp.path() / "state" / "wordsmith" / "session.json";

    wordsmith::ProjectSession written;
    written.root          = root.string();
    written.open_document = "manuscript/act-one/scene.md";
    written.expanded      = {"manuscript"};

    std::string error;
    check(wordsmith::save_project_session(written, file, error),
          "saving creates the directories it needs: " + error);

    const std::string contents = read_file(file);
    check(contents.find("\"projects\"") != std::string::npos,
          "the projects are written under a named key");
    check(contents.find("\"open-document\"") != std::string::npos,
          "the open document is written under a named key");
    check(contents.find("\"version\"") != std::string::npos,
          "the format version is written");
    check(contents.find("\"manuscript/act-one/scene.md\"") != std::string::npos,
          "paths inside the project are written relative to its root");
    check(!contents.empty() && contents.back() == '\n',
          "the file ends with a newline");
}

void test_bad_files_read_as_nothing()
{
    TempDir temp;
    const fs::path root = make_project(temp, "novel");

    const fs::path malformed = temp.path() / "malformed.json";
    write_file(malformed, "{\"projects\": [");
    check_equal(wordsmith::session_for(malformed, root).expanded.size(), 0u,
                "truncated JSON reads as no saved state");

    const fs::path not_an_object = temp.path() / "array.json";
    write_file(not_an_object, "[1, 2, 3]\n");
    check_equal(wordsmith::session_for(not_an_object, root).expanded.size(), 0u,
                "a JSON array reads as no saved state");

    const fs::path wrong_types = temp.path() / "wrong-types.json";
    write_file(wrong_types,
               "{\"projects\": [{\"root\": 7}, {\"open-document\": \"x.md\"}, "
               "{\"root\": \"/p\", \"expanded\": \"manuscript\"}]}\n");
    check_equal(wordsmith::load_session(wrong_types).size(), 1u,
                "entries without a usable root are skipped");
    check_equal(wordsmith::load_session(wrong_types).front().expanded.size(), 0u,
                "an expanded list that is not a list reads as empty");
}

/* An unbounded file would be the price of never forgetting a project, and
 * forgetting the one opened longest ago costs a collapsed binder. */
void test_the_oldest_projects_fall_off()
{
    TempDir temp;
    const fs::path file = temp.path() / "session.json";

    std::string error;
    for (std::size_t index = 0; index < wordsmith::SESSION_PROJECT_LIMIT + 10; index++) {
        wordsmith::ProjectSession session;
        session.root = (temp.path() / ("project-" + std::to_string(index))).string();
        wordsmith::save_project_session(session, file, error);
    }

    const std::vector<wordsmith::ProjectSession> saved = wordsmith::load_session(file);
    check_equal(saved.size(), wordsmith::SESSION_PROJECT_LIMIT,
                "the list is capped at the limit");
    if (!saved.empty()) {
        check_equal(
            saved.front().root,
            wordsmith::session_key(temp.path()
                                   / ("project-"
                                      + std::to_string(wordsmith::SESSION_PROJECT_LIMIT + 9)))
                .string(),
            "the most recently saved project is still at the front");
    }
}

void test_paths_convert_both_ways()
{
    const fs::path root("/books/novel");

    check_equal(wordsmith::session_relative(root, "/books/novel/manuscript/scene.md"),
                "manuscript/scene.md", "a path inside the project is made relative");
    check_equal(wordsmith::session_relative(root, "/books/other/scene.md"), "",
                "a path outside the project is refused");
    check_equal(wordsmith::session_relative(root, root), "",
                "the root itself is not a path within itself");
    check_equal(wordsmith::session_relative(root, ""), "",
                "an empty path converts to nothing");

    check_equal(wordsmith::session_absolute(root, "manuscript/scene.md").string(),
                "/books/novel/manuscript/scene.md",
                "a relative path is resolved against the root");
    check(wordsmith::session_absolute(root, "").empty(),
          "an empty relative path resolves to nothing");

    /* Resolution uses the root as spelled, so a restored path matches the ones
     * the binder is showing rather than a tidier version of them. */
    check_equal(wordsmith::session_absolute("/books/./novel", "manuscript").string(),
                "/books/./novel/manuscript",
                "resolution keeps the caller's spelling of the root");
}

/* The location follows XDG, and it is the state directory rather than the
 * config one: this is restored without being asked for, not configured. */
void test_path_follows_xdg()
{
    const char* state_home_value = std::getenv("XDG_STATE_HOME");
    const char* home_value = std::getenv("HOME");
    const bool  had_state_home = state_home_value != nullptr;
    const bool  had_home = home_value != nullptr;
    const std::string saved_state_home = had_state_home ? state_home_value : "";
    const std::string saved_home = had_home ? home_value : "";

    ::setenv("XDG_STATE_HOME", "/somewhere/state", 1);
    check(wordsmith::session_path() == fs::path("/somewhere/state/wordsmith/session.json"),
          "XDG_STATE_HOME decides where the file lives");

    ::unsetenv("XDG_STATE_HOME");
    ::setenv("HOME", "/home/author", 1);
    check(wordsmith::session_path()
              == fs::path("/home/author/.local/state/wordsmith/session.json"),
          "without XDG_STATE_HOME the file lands under ~/.local/state");

    ::setenv("XDG_STATE_HOME", "", 1);
    check(wordsmith::session_path()
              == fs::path("/home/author/.local/state/wordsmith/session.json"),
          "an empty XDG_STATE_HOME falls back to ~/.local/state");

    if (had_state_home) {
        ::setenv("XDG_STATE_HOME", saved_state_home.c_str(), 1);
    } else {
        ::unsetenv("XDG_STATE_HOME");
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
    test_missing_file_gives_nothing();
    test_round_trip();
    test_the_panes_are_remembered();
    test_projects_are_kept_apart();
    test_saving_again_supersedes_and_promotes();
    test_the_key_is_normalised();
    test_stale_paths_are_dropped();
    test_saved_file_is_json_an_author_can_read();
    test_bad_files_read_as_nothing();
    test_the_oldest_projects_fall_off();
    test_paths_convert_both_ways();
    test_path_follows_xdg();

    return failures == 0 ? 0 : 1;
}
