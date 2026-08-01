#include "core/project.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    fs::path folder;
    check(project->create_folder(manuscript, "Part One", folder, error),
          "create folder: succeeds (" + error + ")");
    check(fs::is_directory(manuscript / "Part-One"),
          "create folder: name is sanitised");
    check(folder == manuscript / "Part-One",
          "create folder: reports where it landed");

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

    fs::path created;
    check(!project->create_folder(temp.path(), "escape", created, error),
          "confinement: a sibling directory is rejected");
    check(!project->create_folder(project->manuscript_path() / ".." / "..",
                                  "escape", created, error),
          "confinement: traversal through .. is rejected");
    check(project->contains(project->manuscript_path()),
          "confinement: the manuscript root itself is allowed");
}

/* The list may only reorder what the scan found. Every check here is really
 * one clause of that rule. */
void test_child_order_is_a_hint()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "order: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    fs::path made;
    project->create_document(manuscript, "apple", made, error);
    project->create_document(manuscript, "banana", made, error);
    project->create_document(manuscript, "cherry", made, error);
    project->create_folder(manuscript, "notes", made, error);

    /* Alphabetical, folders first, until something says otherwise. */
    auto names = [](const wordsmith::BinderEntry& root) {
        std::string joined;
        for (const wordsmith::BinderEntry& child : root.children) {
            joined += (joined.empty() ? "" : ",") + child.name;
        }
        return joined;
    };

    check_equal(names(wordsmith::load_binder(manuscript)),
                "notes,apple,banana,cherry", "order: alphabetical by default");

    check(project->set_child_order(manuscript,
                                   { "cherry.md", "notes", "banana.md" }, error),
          "order: writing succeeds (" + error + ")");
    check(fs::is_regular_file(manuscript / "metadata.yaml"),
          "order: the sidecar is written into the folder it describes");

    check_equal(names(wordsmith::load_binder(manuscript)),
                "cherry,notes,banana,apple",
                "order: listed items lead, the rest follow alphabetically");

    /* Deleting outside Wordsmith: the entry is stale, and simply ignored. */
    fs::remove(manuscript / "cherry.md");
    check_equal(names(wordsmith::load_binder(manuscript)), "notes,banana,apple",
                "order: a name with nothing behind it is dropped");

    /* Appearing outside Wordsmith: unlisted, so it lands at the end. */
    std::string ignored;
    wordsmith::write_document(manuscript / "damson.md", "", ignored);
    check_equal(names(wordsmith::load_binder(manuscript)),
                "notes,banana,apple,damson", "order: a new arrival goes last");

    /* Hand-written lists should not have to know about the extension. */
    project->set_child_order(manuscript, { "damson", "banana" }, error);
    check_equal(names(wordsmith::load_binder(manuscript)),
                "damson,banana,notes,apple", "order: a bare name matches a stem");

    check(!project->set_child_order(temp.path(), { "escape" }, error),
          "order: a folder outside the manuscript is rejected");
}

void test_child_order_preserves_the_sidecar()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "sidecar: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    const fs::path sidecar = manuscript / "metadata.yaml";
    wordsmith::write_document(sidecar,
                              "# How this part hangs together.\n"
                              "synopsis: |-\n"
                              "  The voyage out.\n"
                              "  Everyone is still hopeful.\n"
                              "status: draft\n",
                              error);

    check(project->set_child_order(manuscript, { "a.md", "b.md" }, error),
          "sidecar: order written alongside the fields (" + error + ")");

    std::string text;
    wordsmith::read_document(sidecar, text, error);
    check_equal(text,
                "# How this part hangs together.\n"
                "synopsis: |-\n"
                "  The voyage out.\n"
                "  Everyone is still hopeful.\n"
                "status: draft\n"
                "children:\n"
                "  - a.md\n"
                "  - b.md\n",
                "sidecar: the folder's own fields and comments survive");

    check(wordsmith::read_child_order(manuscript).size() == 2,
          "sidecar: the order reads back");

    project->set_child_order(manuscript, { "b.md" }, error);
    wordsmith::read_document(sidecar, text, error);
    check(text.find("The voyage out.") != std::string::npos
              && text.find("- a.md") == std::string::npos,
          "sidecar: rewriting the order does not disturb the synopsis");
}

void test_move_entry()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "move: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    fs::path folder;
    fs::path nested;
    fs::path document;
    project->create_folder(manuscript, "Part One", folder, error);
    project->create_folder(folder, "Chapters", nested, error);
    project->create_document(manuscript, "Opening", document, error);

    fs::path moved;
    check(project->move_entry(document, folder, moved, error),
          "move: a document into a folder (" + error + ")");
    check(moved == folder / "Opening.md", "move: reports the new path");
    check(fs::is_regular_file(moved) && !fs::exists(document),
          "move: the file actually moved");

    /* The case "New Folder with Selection" leans on: a folder moving into a
     * sibling folder created beside it. */
    fs::path box;
    project->create_folder(manuscript, "Box", box, error);
    fs::path moved_folder;
    check(project->move_entry(folder, box, moved_folder, error),
          "move: a folder into another folder (" + error + ")");
    check(fs::is_directory(box / "Part-One" / "Chapters"),
          "move: the subtree comes along");

    check(!project->move_entry(box, box / "Part-One", moved, error),
          "move: a folder cannot go inside itself");
    check(!project->move_entry(box, box, moved, error),
          "move: a folder cannot go inside itself directly");

    fs::path duplicate_source;
    project->create_document(manuscript, "Opening", duplicate_source, error);
    check(!project->move_entry(duplicate_source, box / "Part-One", moved, error),
          "move: refuses to overwrite a name already there");

    check(!project->move_entry(manuscript, box, moved, error),
          "move: the manuscript root cannot be moved");
    check(!project->move_entry(temp.path() / "outside.md", box, moved, error),
          "move: a source outside the manuscript is rejected");
    check(!project->move_entry(duplicate_source, temp.path(), moved, error),
          "move: a target outside the manuscript is rejected");
}

void test_move_maintains_child_order()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "move order: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    fs::path chapter;
    fs::path part;
    project->create_document(manuscript, "chapter", chapter, error);
    project->create_document(manuscript, "other", part, error);
    project->create_folder(manuscript, "part", part, error);

    project->set_child_order(manuscript, { "chapter.md", "other.md", "part" }, error);
    project->set_child_order(part, {}, error);

    fs::path moved;
    check(project->move_entry(chapter, part, moved, error),
          "move order: the move succeeds (" + error + ")");

    const std::vector<std::string> from = wordsmith::read_child_order(manuscript);
    check(std::find(from.begin(), from.end(), "chapter.md") == from.end(),
          "move order: the old folder forgets it");
    check(from.size() == 2, "move order: the other entries stay");

    const std::vector<std::string> to = wordsmith::read_child_order(part);
    check(to.size() == 1 && to[0] == "chapter.md",
          "move order: the new folder records it");

    /* A folder recording no order gets none written: moving something in must
     * not scatter sidecars through folders nobody has arranged. */
    fs::path plain;
    project->create_folder(manuscript, "plain", plain, error);
    fs::path second;
    project->create_document(manuscript, "second", second, error);
    project->move_entry(second, plain, moved, error);
    check(!fs::exists(plain / "metadata.yaml"),
          "move order: an unarranged folder stays free of a sidecar");
}

/* The names of a folder's children in the order the binder would show them. */
std::string binder_order_of(const fs::path& folder)
{
    const wordsmith::BinderEntry tree = wordsmith::load_binder(folder);
    std::string out;
    for (const wordsmith::BinderEntry& child : tree.children) {
        if (!out.empty()) {
            out += " ";
        }
        out += child.path.filename().string();
    }
    return out;
}

void test_move_beside_reorders()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "beside: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    fs::path made;
    project->create_document(manuscript, "one", made, error);
    project->create_document(manuscript, "two", made, error);
    project->create_document(manuscript, "three", made, error);

    /* Nothing arranged yet, so the alphabetical order is what the drag rearranges
     * — and the whole of it has to be written down, not just the two names the
     * drag mentioned. */
    fs::path moved;
    check(project->move_entry_beside(manuscript / "two.md", manuscript / "one.md",
                                     false, moved, error),
          "beside: dropping above the first item succeeds (" + error + ")");
    check_equal(binder_order_of(manuscript), "two.md one.md three.md",
                "beside: the item lands above the anchor");

    check(project->move_entry_beside(manuscript / "two.md", manuscript / "three.md",
                                     true, moved, error),
          "beside: dropping below the last item succeeds (" + error + ")");
    check_equal(binder_order_of(manuscript), "one.md three.md two.md",
                "beside: the item lands below the anchor");

    /* Dropped on itself: no move, and no rewrite that could shuffle anything. */
    check(project->move_entry_beside(manuscript / "two.md", manuscript / "two.md",
                                     false, moved, error),
          "beside: dropping an item on itself succeeds (" + error + ")");
    check_equal(binder_order_of(manuscript), "one.md three.md two.md",
                "beside: dropping an item on itself changes nothing");

    check(!project->move_entry_beside(manuscript / "one.md", temp.path() / "away.md",
                                      false, moved, error),
          "beside: an anchor outside the manuscript is rejected");
}

void test_move_beside_across_folders()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "beside across: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    fs::path part;
    fs::path made;
    project->create_folder(manuscript, "part", part, error);
    project->create_document(part, "alpha", made, error);
    project->create_document(part, "beta", made, error);
    project->create_document(manuscript, "stray", made, error);

    fs::path moved;
    check(project->move_entry_beside(manuscript / "stray.md", part / "alpha.md",
                                     false, moved, error),
          "beside across: the move succeeds (" + error + ")");
    check(moved == part / "stray.md" && fs::is_regular_file(moved),
          "beside across: the item arrives in the anchor's folder");
    check_equal(binder_order_of(part), "stray.md alpha.md beta.md",
                "beside across: it takes the position it was dropped at");

    /* The folder it left is rearranged only if it was arranged to begin with. */
    check(!fs::exists(manuscript / "metadata.yaml"),
          "beside across: the folder left behind gains no sidecar");

    /* A folder still cannot swallow itself, whichever end the drag names. */
    check(!project->move_entry_beside(part, part / "alpha.md", false, moved, error),
          "beside across: a folder cannot be dropped inside itself");
}

void test_group_into_new_folder()
{
    TempDir temp;
    std::string error;
    auto project = wordsmith::Project::create(temp.path() / "book", "Book", error);
    if (project == nullptr) {
        check(false, "group: setup failed (" + error + ")");
        return;
    }

    const fs::path manuscript = project->manuscript_path();
    fs::path made;
    project->create_document(manuscript, "one", made, error);
    project->create_document(manuscript, "two", made, error);
    project->create_document(manuscript, "three", made, error);
    project->set_child_order(manuscript, { "one.md", "two.md", "three.md" }, error);

    fs::path folder;
    fs::path moved;
    check(project->group_into_new_folder(manuscript / "two.md", "Part Two", folder,
                                         moved, error),
          "group: succeeds (" + error + ")");
    check(fs::is_directory(folder) && folder.filename() == "Part-Two",
          "group: the folder is created beside the item");
    check(moved == folder / "two.md" && fs::is_regular_file(moved),
          "group: the item is inside it");

    /* The group stands where the thing it gathered used to stand. */
    const std::vector<std::string> order = wordsmith::read_child_order(manuscript);
    check(order.size() == 3 && order[0] == "one.md" && order[1] == "Part-Two"
              && order[2] == "three.md",
          "group: the new folder takes the item's place in the order");

    /* With nothing arranged, nothing is written. */
    TempDir plain_temp;
    auto plain = wordsmith::Project::create(plain_temp.path() / "book", "Book", error);
    plain->create_document(plain->manuscript_path(), "solo", made, error);
    check(plain->group_into_new_folder(made, "Box", folder, moved, error),
          "group: succeeds without a sidecar (" + error + ")");
    check(!fs::exists(plain->manuscript_path() / "metadata.yaml"),
          "group: an unarranged folder gains no sidecar");

    check(!project->group_into_new_folder(temp.path() / "outside.md", "Box", folder,
                                          moved, error),
          "group: an item outside the manuscript is rejected");
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
    test_child_order_is_a_hint();
    test_child_order_preserves_the_sidecar();
    test_move_entry();
    test_move_maintains_child_order();
    test_move_beside_reorders();
    test_move_beside_across_folders();
    test_group_into_new_folder();
    test_document_read_write();
    test_sanitize_name();

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
