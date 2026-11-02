// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

// Euclid includes
#include <euclid/core/ObjectCipher.h>

namespace Euclid::ESM {

    /**
     * @brief What is wrong with an object's file, as CheckObjectFile() found it.
     */
    struct ObjectFileProblem {

        /**
         * @brief "missing", "empty", "short", "long", "mismatch" (an encrypted file whose header
         * records a different length than the object does) or "unreadable".
         */
        std::string kind;

        /**
         * @brief The file's size on disk; -1 when there is no file.
         */
        std::int64_t fileSize{-1};

        /**
         * @brief The size the file should have.
         */
        std::int64_t expectedSize{};

        /**
         * @brief Why, for "unreadable".
         */
        std::string detail;
    };

    /**
     * @brief Whether an object's file is the one its record describes.
     *
     * @par
     * By size, not by content: reading every byte back to compare a checksum is a second copy of
     * the whole store's I/O, and what this exists to find - a file a crash left empty or cut short,
     * or one that went missing - shows in its length. A file the right length with the wrong bytes
     * in it is the one case it does not catch.
     *
     * @par
     * In the clear the file is exactly the object's recorded size. Encrypted, it is what its own
     * header says it must be - see Core::ObjectCipher::ExpectedFileSize() - and that header has to
     * record the object's size.
     *
     * @param path         the object's file
     * @param recordedSize the size the object's record gives
     * @param encrypted    whether the object is stored encrypted
     * @return the problem, or nothing if the file is whole
     */
    inline std::optional<ObjectFileProblem> CheckObjectFile(const std::filesystem::path &path, const std::int64_t recordedSize, const bool encrypted) {

        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) return ObjectFileProblem{.kind = "missing", .expectedSize = recordedSize};
            return ObjectFileProblem{.kind = "unreadable", .expectedSize = recordedSize, .detail = ec.message()};
        }
        const auto fileSize = static_cast<std::int64_t>(size);

        std::int64_t expected = recordedSize;
        if (encrypted) {
            if (fileSize == 0) return ObjectFileProblem{.kind = "empty", .fileSize = 0, .expectedSize = recordedSize};
            try {
                if (const auto plaintext = Core::ObjectCipher::PlaintextSize(path); plaintext != recordedSize) {
                    return ObjectFileProblem{.kind = "mismatch", .fileSize = fileSize, .expectedSize = recordedSize,
                                             .detail = "the file's header records " + std::to_string(plaintext) + " bytes"};
                }
                expected = Core::ObjectCipher::ExpectedFileSize(path);
            } catch (const std::exception &e) {
                return ObjectFileProblem{.kind = "unreadable", .fileSize = fileSize, .expectedSize = recordedSize, .detail = e.what()};
            }
        }

        if (fileSize == expected) return std::nullopt;
        if (fileSize == 0) return ObjectFileProblem{.kind = "empty", .fileSize = 0, .expectedSize = expected};
        return ObjectFileProblem{.kind = fileSize < expected ? "short" : "long", .fileSize = fileSize, .expectedSize = expected};
    }

}// namespace Euclid::ESM
