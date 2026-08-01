#include "core/snapshots.hpp"

#include "core/project.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
        std::cerr << "FAIL: " << what << "\n  expected: [" << expected
                  << "]\n  actual:   [" << actual << "]\n";
        failures++;
    }
}

void check_count(std::size_t actual, std::size_t expected, const std::string& what)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << what << "\n  expected: " << expected
                  << "\n  actual:   " << actual << '\n';
        failures++;
    }
}

/* A scratch directory removed on destruction, so a failing check does not
 * leave litter behind in the build tree. */
class TempDir {
public:
    TempDir()
    {
        std::error_code code;
        root_ = fs::temp_directory_path(code)
            / ("wordsmith-snapshot-" + std::to_string(::getpid()) + "-"
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

/* A project directory with a manuscript folder, built by hand rather than
 * through `Project::create` so these tests fail for snapshot reasons only. */
class TempProject {
public:
    TempProject()
    {
        std::error_code code;
        fs::create_directories(manuscript(), code);
        std::ofstream(root() / wordsmith::PROJECT_FILE_NAME) << "{}\n";
    }

    const fs::path& root() const { return dir_.path(); }
    fs::path manuscript() const { return root() / "manuscript"; }

private:
    TempDir dir_;
};

void write_file(const fs::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read_back(const fs::path& path)
{
    std::ifstream      stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

constexpr std::chrono::seconds NO_COOLDOWN{0};

/* ── locating ────────────────────────────────────────────────────────────── */

void test_finds_the_project_root_from_a_nested_document()
{
    TempProject project;
    const fs::path nested = project.manuscript() / "act-one";
    std::error_code code;
    fs::create_directories(nested, code);
    const fs::path document = nested / "scene.md";
    write_file(document, "text");

    check(wordsmith::find_project_root(document) == project.root(),
          "the root is found by walking up from a nested document");
}

void test_no_root_outside_a_project()
{
    TempDir loose;
    const fs::path document = loose.path() / "stray.md";
    write_file(document, "text");

    check(wordsmith::find_project_root(document).empty(),
          "a document outside a project has no root");
    check(wordsmith::snapshot_directory(document).empty(),
          "and so has nowhere to keep snapshots");
    check(!wordsmith::capture_snapshot(document, 5, NO_COOLDOWN),
          "and captures nothing");
}

void test_snapshots_mirror_the_project_layout()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";
    write_file(document, "text");

    const fs::path expected = project.root() / ".wordsmith" / "snapshots"
        / "manuscript" / "scene.md";
    check(wordsmith::snapshot_directory(document) == expected,
          "a document's versions sit at its mirrored path");
}

/* ── capturing ───────────────────────────────────────────────────────────── */

void test_captures_the_previous_contents()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";
    write_file(document, "the first draft");

    check(wordsmith::capture_snapshot(document, 5, NO_COOLDOWN),
          "the first capture stores a version");

    const std::vector<fs::path> kept = wordsmith::snapshots(document);
    check_count(kept.size(), 1, "one version is kept");
    if (!kept.empty()) {
        check_equal(read_back(kept.front()), "the first draft",
                    "the version holds what was on disk");
    }
}

void test_a_missing_document_has_nothing_to_preserve()
{
    TempProject project;
    const fs::path document = project.manuscript() / "not-yet.md";

    check(!wordsmith::capture_snapshot(document, 5, NO_COOLDOWN),
          "a document being created captures nothing");
    check_count(wordsmith::snapshots(document).size(), 0, "and keeps no versions");
}

void test_identical_contents_are_not_stored_twice()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";
    write_file(document, "unchanged");

    check(wordsmith::capture_snapshot(document, 5, NO_COOLDOWN),
          "the first capture stores a version");
    check(!wordsmith::capture_snapshot(document, 5, NO_COOLDOWN),
          "capturing the same contents again stores nothing");
    check_count(wordsmith::snapshots(document).size(), 1, "still one version");
}

void test_the_cooldown_folds_a_burst_into_one_version()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";
    write_file(document, "before the sitting");

    check(wordsmith::capture_snapshot(document, 5, std::chrono::seconds{600}),
          "the first capture stores a version");

    /* Five writes in quick succession, as an inspector interaction makes. */
    for (int edit = 0; edit < 5; edit++) {
        write_file(document, "edit " + std::to_string(edit));
        check(!wordsmith::capture_snapshot(document, 5, std::chrono::seconds{600}),
              "a write inside the cooldown stores nothing");
    }

    const std::vector<fs::path> kept = wordsmith::snapshots(document);
    check_count(kept.size(), 1, "the burst left one version");
    if (!kept.empty()) {
        check_equal(read_back(kept.front()), "before the sitting",
                    "and it is the one from before the sitting");
    }
}

void test_keeps_only_the_newest_five()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";

    for (int version = 0; version < 8; version++) {
        write_file(document, "draft " + std::to_string(version));
        check(wordsmith::capture_snapshot(document, 5, NO_COOLDOWN),
              "each distinct version is stored");
    }

    const std::vector<fs::path> kept = wordsmith::snapshots(document);
    check_count(kept.size(), 5, "five versions are kept");
    if (kept.size() == 5) {
        check_equal(read_back(kept.front()), "draft 3", "the oldest kept is draft 3");
        check_equal(read_back(kept.back()), "draft 7", "the newest kept is draft 7");
    }
}

void test_versions_are_ordered_oldest_first()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";

    for (int version = 0; version < 3; version++) {
        write_file(document, "draft " + std::to_string(version));
        wordsmith::capture_snapshot(document, 5, NO_COOLDOWN);
    }

    const std::vector<fs::path> kept = wordsmith::snapshots(document);
    check_count(kept.size(), 3, "three versions are kept");
    for (std::size_t index = 0; index + 1 < kept.size(); index++) {
        check(kept[index].filename() < kept[index + 1].filename(),
              "names sort oldest first");
    }
}

/* ── through the write path ──────────────────────────────────────────────── */

void test_writing_a_document_snapshots_it_first()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";
    write_file(document, "the draft worth keeping");

    std::string error;
    check(wordsmith::write_document(document, "a bad round trip", error),
          "the write succeeds");
    check_equal(read_back(document), "a bad round trip", "the new contents landed");

    const std::vector<fs::path> kept = wordsmith::snapshots(document);
    check_count(kept.size(), 1, "the write left a version behind");
    if (!kept.empty()) {
        check_equal(read_back(kept.front()), "the draft worth keeping",
                    "and it is what the write replaced");
    }
}

void test_creating_a_document_leaves_no_version()
{
    TempProject project;
    const fs::path document = project.manuscript() / "new.md";

    std::string error;
    check(wordsmith::write_document(document, "", error), "the write succeeds");
    check_count(wordsmith::snapshots(document).size(), 0,
                "a document with no history gets no version");
}

void test_snapshots_stay_out_of_the_binder()
{
    TempProject project;
    const fs::path document = project.manuscript() / "scene.md";
    write_file(document, "text");
    wordsmith::capture_snapshot(document, 5, NO_COOLDOWN);

    const wordsmith::BinderEntry root = wordsmith::load_binder(project.manuscript());
    check_count(root.children.size(), 1, "the binder shows only the document");
    if (!root.children.empty()) {
        check_equal(root.children.front().name, "scene", "and it is the document");
    }
}

} // namespace

int main()
{
    test_finds_the_project_root_from_a_nested_document();
    test_no_root_outside_a_project();
    test_snapshots_mirror_the_project_layout();

    test_captures_the_previous_contents();
    test_a_missing_document_has_nothing_to_preserve();
    test_identical_contents_are_not_stored_twice();
    test_the_cooldown_folds_a_burst_into_one_version();
    test_keeps_only_the_newest_five();
    test_versions_are_ordered_oldest_first();

    test_writing_a_document_snapshots_it_first();
    test_creating_a_document_leaves_no_version();
    test_snapshots_stay_out_of_the_binder();

    return failures == 0 ? 0 : 1;
}
