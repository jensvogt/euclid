//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <filesystem>
#include <string>

namespace Euclid::Core {

    /**
     * @brief Where a file whose name is a generated ID lives on disk.
     *
     * @par
     * A storage module names each file after a fresh UUID and keeps the mapping from that name to
     * a bucket and key in the database, so nothing about the directory has to be browsable. Put
     * every one of those files in a single directory, though, and the directory itself becomes the
     * limit long before the filesystem does: at around nine million entries, ext4's hashed
     * directory index reaches its maximum depth and returns ENOSPC - "No space left on device" -
     * for the fraction of new names whose hash lands in a full leaf, while df still reports
     * terabytes free. What that looks like from the outside is uploads failing at random for no
     * stated reason.
     *
     * @par
     * So the name is fanned out over two levels taken from its own first characters:
     * "96719be3-..." is stored as "objects/96/71/96719be3-...". That is 65,536 directories, and it
     * is the whole trick - the name already is a uniformly distributed hash, so nothing has to be
     * computed and the path can be derived again from the name alone, with no index anywhere.
     *
     * @par
     * Files written before any of this existed are still where they were. FindFile() answers with
     * the fanned-out path when there is a file there and the flat one otherwise, so old objects
     * keep being served with no migration, and RemoveFile() looks in both for the same reason.
     * That also means the two layouts can coexist indefinitely: nothing has to be moved for the
     * installation to keep working, and anything that is moved keeps working too.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class DirUtils {

    public:

        /**
         * @brief Directory the fanned-out tree is rooted at, below the storage's own directory.
         *
         * @par
         * A subdirectory rather than the storage root itself, because on a directory that has
         * already hit the index limit, adding the fan-out at the top would mean creating up to 256
         * new entries in exactly the directory that can no longer reliably take them. This is one
         * entry, and everything underneath it is in fresh directories with room to spare.
         */
        static constexpr auto kObjectDir = "objects";

        /**
         * @brief How many characters of the name each level of the fan-out takes.
         */
        static constexpr std::size_t kShardLength = 2;

        /**
         * @brief How many levels deep the fan-out goes. Two levels of two hexadecimal characters
         * is 65,536 directories - about 140 files each at nine million objects, and a directory
         * that size is one no filesystem has an opinion about.
         */
        static constexpr std::size_t kShardLevels = 2;

        /**
         * @brief The fanned-out path a name maps to, without touching the filesystem.
         *
         * @param rootDir the storage's own directory, e.g. euclid.modules.esm.data-dir.
         * @param name the file's name, normally a generated UUID.
         * @return rootDir/objects/xx/yy/name, or rootDir/name for a name too short to fan out or
         * one that is not a plain filename - a name carrying a separator or ".." is refused the
         * fan-out rather than allowed to build a path outside the storage.
         */
        [[nodiscard]]
        static std::filesystem::path ShardedFilePath(const std::filesystem::path &rootDir, const std::string &name);

        /**
         * @brief The path to write a new file to, with the directory that holds it created.
         *
         * @par
         * Falls back to the flat path if those directories cannot be created, so a storage that
         * has run out of room in its root is left no worse off than it already was rather than
         * refusing writes outright. The reason is logged either way.
         *
         * @param rootDir the storage's own directory.
         * @param name the file's name.
         * @return the path to write to.
         */
        [[nodiscard]]
        static std::filesystem::path CreateFilePath(const std::filesystem::path &rootDir, const std::string &name);

        /**
         * @brief Where an existing file is: the fanned-out path when a file is there, the flat one
         * otherwise.
         *
         * @par
         * The flat answer is also what a caller gets for a file that does not exist at all, which
         * is what makes this usable as "the path this file would be read from" - the caller opens
         * it and reports the failure with the path it tried, as it always did.
         *
         * @param rootDir the storage's own directory.
         * @param name the file's name.
         * @return the path to read from.
         */
        [[nodiscard]]
        static std::filesystem::path FindFilePath(const std::filesystem::path &rootDir, const std::string &name);

        /**
         * @brief Removes a file from wherever it is, fanned out or flat.
         *
         * @par
         * Both are tried rather than only the one FindFilePath() names: a file that somehow exists
         * in both places would otherwise leave the second copy behind forever, unreferenced and
         * invisible.
         *
         * @param rootDir the storage's own directory.
         * @param name the file's name.
         * @param ec receives the error of the last removal that failed, and is cleared otherwise.
         * @return true if a file was removed.
         */
        static bool RemoveFile(const std::filesystem::path &rootDir, const std::string &name, std::error_code &ec);
    };

}// namespace Euclid::Core
