#include "safe-write.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace wordsmith {

namespace {

std::string reason(int number)
{
    return std::string(std::strerror(number));
}

fs::path temporary_beside(const fs::path& target, unsigned attempt)
{
    static std::atomic<unsigned> counter{0};

    const std::string name = "." + target.filename().string() + ".wordsmith-"
        + std::to_string(::getpid()) + "-"
        + std::to_string(counter.fetch_add(1)) + "-" + std::to_string(attempt);

    const fs::path parent = target.parent_path();
    return (parent.empty() ? fs::path(".") : parent) / name;
}

/* write(2) is allowed to write less than it was asked to, and to be cut short
 * by a signal. Neither is an error; stopping early would be. */
bool write_all(int fd, std::string_view bytes)
{
    const char* cursor = bytes.data();
    std::size_t left   = bytes.size();

    while (left > 0) {
        const ssize_t written = ::write(fd, cursor, left);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += written;
        left -= static_cast<std::size_t>(written);
    }
    return true;
}

/* A directory's own contents are metadata like any other, so the rename needs
 * flushing separately from the bytes it published. */
bool fsync_directory(const fs::path& directory, std::string& error)
{
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + directory.string() + ": " + reason(errno);
        return false;
    }

    const bool ok = ::fsync(fd) == 0;
    const int  failure = errno;
    ::close(fd);

    if (!ok) {
        error = "cannot flush " + directory.string() + ": " + reason(failure);
    }
    return ok;
}

void discard(int fd, const fs::path& temporary)
{
    if (fd >= 0) {
        ::close(fd);
    }
    std::error_code code;
    fs::remove(temporary, code);
}

} // namespace

bool write_file_atomically(const fs::path& path, std::string_view bytes,
                           std::string& error)
{
    std::error_code code;

    fs::path target = path;
    if (fs::is_symlink(target, code)) {
        const fs::path resolved = fs::weakly_canonical(target, code);
        if (!code && !resolved.empty()) {
            target = resolved;
        }
    }

    const fs::path parent    = target.parent_path();
    const fs::path directory = parent.empty() ? fs::path(".") : parent;

    struct ::stat existing {};
    const bool   replacing = ::stat(target.c_str(), &existing) == 0;
    const mode_t mode      = replacing ? (existing.st_mode & 07777) : 0666;

    /* O_EXCL means a name that is somehow already taken is a retry, not a
     * silent clobber of someone else's write in progress. */
    fs::path temporary;
    int      fd = -1;
    for (unsigned attempt = 0; attempt < 8 && fd < 0; attempt++) {
        temporary = temporary_beside(target, attempt);
        fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        if (fd < 0 && errno != EEXIST) {
            error = "cannot write " + target.string() + ": " + reason(errno);
            return false;
        }
    }
    if (fd < 0) {
        error = "cannot write " + target.string() + ": no free temporary name";
        return false;
    }

    /* open() filtered `mode` through the umask, which is right for a new file
     * and wrong for one being replaced. */
    if (replacing && ::fchmod(fd, mode) != 0) {
        error = "cannot set permissions on " + target.string() + ": " + reason(errno);
        discard(fd, temporary);
        return false;
    }

    if (!write_all(fd, bytes)) {
        error = "failed writing " + target.string() + ": " + reason(errno);
        discard(fd, temporary);
        return false;
    }

    /* Before the rename, not after: the swap must not be able to reach the disk
     * ahead of the contents it is publishing. */
    if (::fsync(fd) != 0) {
        error = "failed writing " + target.string() + ": " + reason(errno);
        discard(fd, temporary);
        return false;
    }

    if (::close(fd) != 0) {
        error = "failed writing " + target.string() + ": " + reason(errno);
        discard(-1, temporary);
        return false;
    }

    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        error = "cannot replace " + target.string() + ": " + reason(errno);
        discard(-1, temporary);
        return false;
    }

    /* The contents are safe from here on; only the name might not be. Report a
     * failure, but do not undo the rename — the new file is the good one. */
    return fsync_directory(directory, error);
}

} // namespace wordsmith
