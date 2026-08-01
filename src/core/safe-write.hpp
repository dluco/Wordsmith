//
// Replacing a file without a window in which it is neither the old thing nor
// the new one.
//
// Every write in Wordsmith destroys something: the previous contents of a file
// the author cannot reproduce. `std::ofstream` with `trunc` throws those away
// before the first byte of the replacement lands, so a crash, a full disk, or a
// power cut anywhere in between leaves a truncated chapter. The fix is the
// standard one — write the new contents somewhere else, then swap it in with
// `rename(2)`, which is atomic: a reader sees the whole old file or the whole
// new one, never a prefix.
//
// The somewhere else has to be *beside the target*, not in the system temp
// directory. `rename` only works within a filesystem, and /tmp is usually tmpfs
// while the project may sit on an external drive or a network mount; crossing
// the boundary fails with EXDEV, and the copy-and-delete fallback is exactly
// the non-atomic write we are trying to avoid. So the temp file is a hidden
// sibling, named after its target so an abandoned one is traceable to the write
// that died.
//
// Ordering matters as much as the rename. The bytes are fsync'd before the
// rename, or the metadata operation can reach the disk ahead of the data and a
// crash leaves a file that is atomically zero-length. The *directory* is
// fsync'd after, or the rename itself may not survive. Both are cheap at the
// size of a manuscript document, and both are the difference between "usually
// fine" and "fine".
//
// What this does not protect against, and what should not be mistaken for it:
// this guarantees a write lands whole. It says nothing about whether the bytes
// were right. Wordsmith's editor save is a round trip through `markup.hpp` and
// is not byte-for-byte lossless, so a bug there produces a well-formed, fully
// committed, wrong document. Guarding against that is a different mechanism —
// keeping the previous version — and belongs on top of this one, not inside it.
//

#ifndef WORDSMITH_SAFE_WRITE_HPP
#define WORDSMITH_SAFE_WRITE_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace wordsmith {

/**
 * Replace `path`'s contents with `bytes`, or leave the file exactly as it was.
 * Returns false and fills `error` on failure, having removed any partial work.
 *
 * Creates the file if it does not exist; the parent directory must.
 *
 * Two properties of the existing file are carried across, because the swap
 * replaces the inode rather than the contents and would otherwise quietly
 * change them:
 *
 *   - its permission bits, so a document the author made private stays private.
 *     A file being created instead gets the usual 0666-and-umask.
 *   - its identity as a symlink's destination. A symlink names where the bytes
 *     live, so it is resolved first and the write lands on the far end. Without
 *     that, saving would replace the link with a regular file and detach
 *     whatever it pointed at.
 *
 * Hard links are not preserved — rename gives the target a new inode, so a
 * second link to the old one keeps the old contents. Nothing in Wordsmith
 * creates hard links, but an author who did would be surprised, and the
 * alternative (writing in place) is the unsafety this exists to remove.
 */
bool write_file_atomically(const std::filesystem::path& path, std::string_view bytes,
                           std::string& error);

} // namespace wordsmith

#endif /* WORDSMITH_SAFE_WRITE_HPP */
