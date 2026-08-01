#include "core/safe-write.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/stat.h>
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
            / ("wordsmith-safe-write-" + std::to_string(::getpid()) + "-"
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
    fs::path         root_;
    static inline int counter_ = 0;
};

std::string read_back(const fs::path& path)
{
    std::ifstream  stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void write_file(const fs::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/* Nothing but the target should survive a write — an abandoned temp file beside
 * a chapter is litter the author has to explain to themselves. */
int entries_in(const fs::path& directory)
{
    int count = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        (void)entry;
        count++;
    }
    return count;
}

mode_t mode_of(const fs::path& path)
{
    struct ::stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return 0;
    }
    return info.st_mode & 07777;
}

void test_creates_a_new_file()
{
    TempDir dir;
    const fs::path target = dir.path() / "chapter.md";

    std::string error;
    check(wordsmith::write_file_atomically(target, "hello", error),
          "creating a file succeeds");
    check_equal(read_back(target), "hello", "new file holds the bytes written");
    check(entries_in(dir.path()) == 1, "no temporary file is left behind");
}

void test_replaces_an_existing_file()
{
    TempDir dir;
    const fs::path target = dir.path() / "chapter.md";
    write_file(target, "the old draft");

    std::string error;
    check(wordsmith::write_file_atomically(target, "the new draft", error),
          "replacing a file succeeds");
    check_equal(read_back(target), "the new draft", "the replacement is complete");
    check(entries_in(dir.path()) == 1, "no temporary file is left behind");
}

void test_truncates_when_the_replacement_is_shorter()
{
    TempDir dir;
    const fs::path target = dir.path() / "chapter.md";
    write_file(target, "a much longer previous draft");

    std::string error;
    check(wordsmith::write_file_atomically(target, "short", error),
          "shortening a file succeeds");
    check_equal(read_back(target), "short", "no tail of the old file survives");
}

void test_empty_contents()
{
    TempDir dir;
    const fs::path target = dir.path() / "chapter.md";
    write_file(target, "something");

    std::string error;
    check(wordsmith::write_file_atomically(target, std::string_view(), error),
          "writing nothing succeeds");
    check_equal(read_back(target), "", "the file is emptied");
}

void test_keeps_the_permissions_of_the_file_it_replaces()
{
    TempDir dir;
    const fs::path target = dir.path() / "private.md";
    write_file(target, "for my eyes");
    ::chmod(target.c_str(), 0600);

    std::string error;
    check(wordsmith::write_file_atomically(target, "still for my eyes", error),
          "replacing a private file succeeds");
    check(mode_of(target) == 0600, "the replacement is still 0600");
}

void test_writes_through_a_symlink()
{
    TempDir dir;
    const fs::path real = dir.path() / "real.md";
    const fs::path link = dir.path() / "link.md";
    write_file(real, "original");

    std::error_code code;
    fs::create_symlink(real, link, code);
    if (code) {
        return;   // a filesystem without symlinks has nothing to prove here
    }

    std::string error;
    check(wordsmith::write_file_atomically(link, "updated", error),
          "writing through a symlink succeeds");
    check(fs::is_symlink(link), "the symlink is still a symlink");
    check_equal(read_back(real), "updated", "the bytes landed on the far end");
}

void test_a_missing_directory_fails_without_litter()
{
    TempDir dir;
    const fs::path target = dir.path() / "no-such-folder" / "chapter.md";

    std::string error;
    check(!wordsmith::write_file_atomically(target, "hello", error),
          "writing into a missing directory fails");
    check(!error.empty(), "the failure is explained");
    check(entries_in(dir.path()) == 0, "nothing was created");
}

void test_a_failed_write_leaves_the_original_alone()
{
    TempDir dir;
    const fs::path folder = dir.path() / "locked";
    fs::create_directory(folder);
    const fs::path target = folder / "chapter.md";
    write_file(target, "the draft that must survive");

    /* Read and execute but not write: the temp file cannot be created, which is
     * the failure the whole design exists to make survivable. */
    ::chmod(folder.c_str(), 0500);

    std::string error;
    const bool   wrote = wordsmith::write_file_atomically(target, "clobbered", error);

    ::chmod(folder.c_str(), 0700);

    if (::geteuid() == 0) {
        return;   // root ignores the mode bits, so there is nothing to observe
    }

    check(!wrote, "writing into an unwritable directory fails");
    check_equal(read_back(target), "the draft that must survive",
                "the original is untouched by a failed write");
    check(entries_in(folder) == 1, "no temporary file is left behind");
}

} // namespace

int main()
{
    test_creates_a_new_file();
    test_replaces_an_existing_file();
    test_truncates_when_the_replacement_is_shorter();
    test_empty_contents();
    test_keeps_the_permissions_of_the_file_it_replaces();
    test_writes_through_a_symlink();
    test_a_missing_directory_fails_without_litter();
    test_a_failed_write_leaves_the_original_alone();

    return failures == 0 ? 0 : 1;
}
