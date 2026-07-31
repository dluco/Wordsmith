#include "core/project.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
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
        std::cerr << "FAIL: " << what << "\n  expected: [" << expected
                  << "]\n  actual:   [" << actual << "]\n";
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
            / ("wordsmith-test-" + std::to_string(::getpid()) + "-"
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
    fs::path root_;
    static int counter_;
};

int TempDir::counter_ = 0;

void test_create_and_open()
{
    TempDir temp;
    const fs::path root = temp.path() / "The Long Winter";

    std::string error;
    auto created = wordsmith::Project::create(root, "The Long Winter", error);
    check(created != nullptr, "create: succeeds (" + error + ")");
    if (created == nullptr) {
        return;
    }

    check(fs::is_regular_file(root / "project.wordsmith"),
          "create: writes the project file");
    check(fs::is_directory(root / "manuscript"),
          "create: makes the manuscript folder");

    /* Creating over an existing project must not silently clobber it. */
    std::string second_error;
    check(wordsmith::Project::create(root, "Other", second_error) == nullptr,
          "create: refuses an existing project");

    auto opened = wordsmith::Project::open(root, error);
    check(opened != nullptr, "open: succeeds (" + error + ")");
    if (opened == nullptr) {
        return;
    }
    check_equal(opened->title(), "The Long Winter", "open: reads the title");
    check(opened->binder().children.empty(), "open: a new binder is empty");
}

void test_open_rejects_non_project()
{
    TempDir temp;
    std::string error;
    check(wordsmith::Project::open(temp.path(), error) == nullptr,
          "open: a bare directory is not a project");
    check(!error.empty(), "open: failure fills in an error");
}

void test_open_rejects_malformed_json()
{
    TempDir temp;
    {
        std::ofstream stream(temp.path() / "project.wordsmith");
        stream << "{ this is not json";
    }

    std::string error;
    check(wordsmith::Project::open(temp.path(), error) == nullptr,
          "open: malformed JSON is rejected");
}

void test_binder_ordering_and_filtering()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "binder: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    std::error_code code;
    fs::create_directory(manuscript / "part-two", code);
    fs::create_directory(manuscript / "part-one", code);

    for (const char* name : {"zebra.md", "apple.md", "notes.txt", ".hidden.md"}) {
        std::ofstream stream(manuscript / name);
        stream << "content\n";
    }
    {
        std::ofstream stream(manuscript / "part-one" / "arrival.md");
        stream << "# Arrival\n";
    }

    project->reload_binder();
    const auto& children = project->binder().children;

    /* Folders first, then documents, each alphabetically. The .txt and the
     * dotfile are filtered out entirely. */
    check(children.size() == 4, "binder: four visible entries");
    if (children.size() != 4) {
        return;
    }
    check(children[0].is_folder && children[0].name == "part-one",
          "binder: folders sort first");
    check(children[1].is_folder && children[1].name == "part-two",
          "binder: folders sort alphabetically");
    check_equal(children[2].name, "apple", "binder: documents follow, sorted");
    check_equal(children[3].name, "zebra", "binder: last document");
    check(children[0].children.size() == 1, "binder: recurses into folders");

    /* The display name drops the extension; the path keeps it. */
    check(children[2].path.filename() == "apple.md", "binder: path keeps .md");
}

void test_create_folder_and_document()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "create items: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    check(project->create_folder(manuscript, "Part One", error),
          "create folder: succeeds (" + error + ")");
    check(fs::is_directory(manuscript / "Part-One"),
          "create folder: name is sanitised");

    fs::path created;
    check(project->create_document(manuscript / "Part-One", "The Arrival!",
                                   created, error),
          "create document: succeeds (" + error + ")");
    check(created.filename() == "The-Arrival.md",
          "create document: sanitised, with the extension added");
    check(fs::is_regular_file(created), "create document: file exists");

    check(!project->create_document(manuscript / "Part-One", "The Arrival!",
                                    created, error),
          "create document: refuses a duplicate");
}

/* The mutating calls must not be usable to write outside the manuscript. */
void test_writes_are_confined_to_the_manuscript()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "confinement: setup failed (" + error + ")");
        return;
    }

    check(!project->create_folder(temp.path(), "escape", error),
          "confinement: a sibling directory is rejected");
    check(!project->create_folder(project->manuscript_path() / ".." / "..",
                                  "escape", error),
          "confinement: traversal through .. is rejected");
    check(project->contains(project->manuscript_path()),
          "confinement: the manuscript root itself is allowed");
}

void test_document_read_write()
{
    TempDir temp;
    const fs::path document = temp.path() / "scene.md";

    std::string error;
    check(wordsmith::write_document(document, "# Scene\n\nText.\n", error),
          "document: write succeeds (" + error + ")");

    std::string contents;
    check(wordsmith::read_document(document, contents, error),
          "document: read succeeds (" + error + ")");
    check_equal(contents, "# Scene\n\nText.\n", "document: round trips");

    check(!wordsmith::read_document(temp.path() / "missing.md", contents, error),
          "document: reading a missing file fails");
}

void test_sanitize_name()
{
    check_equal(wordsmith::sanitize_name("The Arrival"), "The-Arrival",
                "sanitize: spaces become hyphens");
    check_equal(wordsmith::sanitize_name("a/b\\c:d"), "a-b-c-d",
                "sanitize: path separators are stripped");
    check_equal(wordsmith::sanitize_name("  padded  "), "padded",
                "sanitize: edges are trimmed");
    check_equal(wordsmith::sanitize_name("!!!"), "untitled",
                "sanitize: nothing usable falls back");
    check_equal(wordsmith::sanitize_name(".hidden"), "hidden",
                "sanitize: cannot produce a dotfile");
    check_equal(wordsmith::sanitize_name("don't"), "don't",
                "sanitize: apostrophes survive");
}

} // namespace

int main()
{
    test_create_and_open();
    test_open_rejects_non_project();
    test_open_rejects_malformed_json();
    test_binder_ordering_and_filtering();
    test_create_folder_and_document();
    test_writes_are_confined_to_the_manuscript();
    test_document_read_write();
    test_sanitize_name();

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
