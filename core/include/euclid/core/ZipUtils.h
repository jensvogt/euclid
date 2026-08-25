//
// Created by vogje01 on 25/08/2026.
//

#pragma once

// C++ includes
#include <filesystem>
#include <string>

// Forward declaration to avoid leaking libarchive's public header onto consumers of this header.
struct archive;

namespace Euclid::Core {

    /**
     * @brief ZIP compression utilities.
     *
     * @par
     * Wraps libarchive's ZIP writer to compress either a single file or an entire directory tree
     * into a single .zip archive.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class ZipUtils {

      public:

        /**
         * @brief Zips a file or a directory.
         *
         * @par
         * If sourcePath is a directory, it is added recursively, with entry names rooted at the
         * directory's own name (e.g. zipping "/tmp/data" produces entries "data/...."), matching
         * the behaviour of the "zip -r" command line tool. If sourcePath is a regular file, the
         * archive contains a single entry with that file's name.
         *
         * @param sourcePath path of the file or directory to zip
         * @param zipFile path of the ZIP archive to create
         * @throws std::runtime_error if sourcePath does not exist, or the archive could not be written
         */
        static void Zip(const std::string &sourcePath, const std::string &zipFile);

      private:

        /**
         * @brief Adds a single regular file to an open ZIP archive.
         *
         * @param writer open libarchive write handle
         * @param filePath path of the file to read from disk
         * @param entryName path to store the file under inside the archive
         */
        static void AddFileEntry(archive *writer, const std::filesystem::path &filePath, const std::string &entryName);
    };

} // namespace Euclid::Core
