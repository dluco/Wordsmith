#include "snapshots.hpp"

#include "project.hpp"
#include "safe-write.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace wordsmith {

namespace {

/* UTC and fixed-width, so the name sorts chronologically and means the same
 * thing to an author who moved timezone between drafts. */
std::string utc_stamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm           utc {};
#if defined(_WIN32)
    ::gmtime_s(&utc, &now);
#else
    ::gmtime_r(&now, &utc);
#endif

    char stamp[32] = {0};
    std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &utc);
    return std::string(stamp);
}

constexpr int MAX_PER_SECOND = 100;

/* The counter separating two versions stored in the same second. It has to keep
 * climbing rather than fill the gap eviction just made: these names are the
 * only record of what order the versions were taken in, so reusing a freed one
 * would silently reorder the history and evict the wrong version next time. */
int next_counter(const fs::path& directory, const std::string& stamp)
{
    const std::string prefix = stamp + "-";
    int               highest = -1;

    std::error_code code;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, code)) {
        const std::string name = entry.path().filename().string();
        if (name.compare(0, prefix.size(), prefix) != 0
            || name.size() < prefix.size() + 2) {
            continue;
        }
        const char tens = name[prefix.size()];
        const char ones = name[prefix.size() + 1];
        if (std::isdigit(static_cast<unsigned char>(tens)) == 0
            || std::isdigit(static_cast<unsigned char>(ones)) == 0) {
            continue;
        }
        highest = std::max(highest, (tens - '0') * 10 + (ones - '0'));
    }

    const int next = highest + 1;
    return next < MAX_PER_SECOND ? next : -1;
}

std::string snapshot_name(const std::string& stamp, int counter,
                          const std::string& extension)
{
    char suffix[8] = {0};
    std::snprintf(suffix, sizeof suffix, "-%02d", counter);
    return stamp + suffix + extension;
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

/* Newer than `cooldown` ago. A snapshot whose timestamp cannot be read counts
 * as old: erring towards keeping one more version than needed. */
bool within_cooldown(const fs::path& snapshot, std::chrono::seconds cooldown)
{
    if (cooldown <= std::chrono::seconds::zero()) {
        return false;
    }

    std::error_code code;
    const fs::file_time_type written = fs::last_write_time(snapshot, code);
    if (code) {
        return false;
    }
    return fs::file_time_type::clock::now() - written < cooldown;
}

void evict_oldest(std::vector<fs::path> kept, int keep)
{
    if (keep < 0) {
        keep = 0;
    }
    if (kept.size() <= static_cast<std::size_t>(keep)) {
        return;
    }

    const std::size_t excess = kept.size() - static_cast<std::size_t>(keep);
    for (std::size_t index = 0; index < excess; index++) {
        std::error_code code;
        fs::remove(kept[index], code);
    }
}

} // namespace

fs::path find_project_root(const fs::path& path)
{
    std::error_code code;
    fs::path        directory = fs::absolute(path, code);
    if (code) {
        return {};
    }

    /* The document itself is never the root, so start at its parent — but a
     * caller handing us a directory should have it considered. */
    if (!fs::is_directory(directory, code)) {
        directory = directory.parent_path();
    }

    while (!directory.empty()) {
        if (fs::exists(directory / PROJECT_FILE_NAME, code)) {
            return directory;
        }
        const fs::path parent = directory.parent_path();
        if (parent == directory) {
            break;   // reached the filesystem root
        }
        directory = parent;
    }
    return {};
}

fs::path snapshot_directory(const fs::path& document)
{
    const fs::path root = find_project_root(document);
    if (root.empty()) {
        return {};
    }

    std::error_code code;
    const fs::path  relative = fs::relative(fs::absolute(document, code), root, code);
    if (code || relative.empty() || *relative.begin() == "..") {
        return {};
    }

    return root / PRIVATE_FOLDER_NAME / SNAPSHOT_FOLDER_NAME / relative;
}

std::vector<fs::path> snapshots(const fs::path& document)
{
    std::vector<fs::path> found;

    const fs::path directory = snapshot_directory(document);
    if (directory.empty()) {
        return found;
    }

    std::error_code code;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, code)) {
        if (entry.is_regular_file(code)) {
            found.push_back(entry.path());
        }
    }

    // The naming does the ordering; see `snapshot_name`.
    std::sort(found.begin(), found.end());
    return found;
}

bool capture_snapshot(const fs::path& document, int keep, std::chrono::seconds cooldown)
{
    if (keep <= 0) {
        return false;
    }

    const fs::path directory = snapshot_directory(document);
    if (directory.empty()) {
        return false;
    }

    /* Nothing on disk yet means nothing to preserve: a document being created
     * has no previous version, and neither does a folder's first sidecar. */
    std::string contents;
    if (!read_file(document, contents)) {
        return false;
    }

    std::vector<fs::path> kept = snapshots(document);
    if (!kept.empty()) {
        const fs::path& newest = kept.back();
        if (within_cooldown(newest, cooldown)) {
            return false;
        }
        std::string previous;
        if (read_file(newest, previous) && previous == contents) {
            return false;
        }
    }

    std::error_code code;
    fs::create_directories(directory, code);
    if (code) {
        return false;
    }

    const std::string stamp   = utc_stamp();
    const int         counter = next_counter(directory, stamp);
    if (counter < 0) {
        return false;   // a hundred versions in one second: something is wrong
    }

    const fs::path candidate =
        directory / snapshot_name(stamp, counter, document.extension().string());

    std::string ignored;
    if (!write_file_atomically(candidate, contents, ignored)) {
        return false;
    }

    kept.push_back(candidate);
    evict_oldest(std::move(kept), keep);
    return true;
}

} // namespace wordsmith
