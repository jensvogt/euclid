// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Euclid::Core {

    /**
     * @brief Where one file is inside a zip written by ArchiveZip - everything needed to read it
     * back without the zip's central directory.
     */
    struct ZipMember {
        /// Offset of the member's local header from the start of the zip.
        std::uint64_t offset = 0;
        /// Bytes the member occupies after its header, compressed or not.
        std::uint64_t compressedSize = 0;
        /// Bytes it extracts to.
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
        /// 0 stored, 8 deflated.
        std::uint16_t method = 0;
    };

    /**
     * @brief A zip file that is only ever appended to - ESM's archive, one per bucket and month.
     *
     * @par
     * Zip rather than tar.gz because a member of a zip can be read on its own, from its offset,
     * where a member of a compressed tar can only be reached by decompressing everything before
     * it; and because a zip is appended to by writing over its central directory, not by
     * rewriting what is already there. Always Zip64, so neither a member nor the archive is held
     * to 4 GB, and the result opens in unzip, 7-Zip, libarchive and the file managers.
     *
     * @par
     * The caller keeps each member's ZipMember and reads it back by that; the central directory
     * is for everybody else. That is also what makes an append safe to interrupt: a member is
     * written with an invalid signature and validated only once its bytes are all there, so an
     * append cut short leaves an archive whose next append finds the last complete member by
     * walking the local headers, drops the rest, and writes a fresh directory.
     *
     * @par
     * Appends to one zip are serialised, within the process and across processes, by a lock file
     * beside it ("<zip>.lock"). Reads take no lock: what they read is never written again.
     */
    class ArchiveZip {

      public:

        /**
         * @brief One entry of the central directory.
         */
        struct Entry {
            std::string name;
            ZipMember member;
        };

        /**
         * @brief Appends a file to a zip, creating the zip and its directory when they are not there.
         *
         * @param zip the archive.
         * @param entryName the name the member has in the archive; need not be unique.
         * @param source the file to add.
         * @param deflate compress it, rather than store it as it is - for bytes that will not
         * compress (encrypted, already compressed) storing is as small and much cheaper.
         * @return where it went.
         * @throws std::runtime_error when the source cannot be read or the archive written.
         */
        static ZipMember Append(const std::filesystem::path &zip, const std::string &entryName, const std::filesystem::path &source, bool deflate);

        /**
         * @brief Writes one member out to a file: under a temporary name beside @p target first,
         * checked against its size and CRC, and only then renamed onto it.
         *
         * @throws std::runtime_error when the member is not where @p member says or does not check out.
         */
        static void Extract(const std::filesystem::path &zip, const ZipMember &member, const std::filesystem::path &target);

        /**
         * @brief The members a zip lists, in the order they were added; a damaged directory is
         * recovered from the local headers, as an append would.
         *
         * @throws std::runtime_error when the zip cannot be read.
         */
        static std::vector<Entry> List(const std::filesystem::path &zip);

        /**
         * @brief Deletes a zip and its lock file, under the lock, so no append is half way into
         * it. Nothing there is not an error.
         *
         * @return true when there was a zip to delete.
         */
        static bool Remove(const std::filesystem::path &zip);
    };

}// namespace Euclid::Core
