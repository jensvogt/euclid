// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// Euclid includes
#include <euclid/core/Configuration.h>

namespace Euclid::ESM {

    /**
     * @brief Where an object's file lives: the directory recorded on the object, or the data
     * directory for an object written before there was a choice.
     *
     * @par
     * The recorded directory is read back as it was written and is never re-derived from the
     * configuration, so taking a filesystem out of euclid.modules.esm.storage-dirs stops new
     * objects going there and leaves every object already on it readable.
     *
     * @param internalDir Object::internalDir; empty for "the data directory".
     * @param dataDir euclid.modules.esm.data-dir.
     */
    inline std::filesystem::path ObjectRoot(const std::string &internalDir, const std::filesystem::path &dataDir) {
        return internalDir.empty() ? dataDir : std::filesystem::path(internalDir);
    }

    /**
     * @brief The bytes free on @p dir's filesystem that euclid can write, or nothing when the
     * directory cannot be created or measured - which takes it out of the rotation.
     */
    using AvailableBytes = std::function<std::optional<std::uintmax_t>(const std::filesystem::path &)>;

    /**
     * @brief Picks the directory a new object goes to, round-robin from @p start.
     *
     * @par
     * The first directory from @p start on that has room for the object and still keeps
     * @p minFreeBytes free after it. A full filesystem is skipped rather than failed on, so one
     * volume filling up does not turn every Nth upload into an error. When none has that much room,
     * the one with the most: the write then reports the full disk with its reason, which is better
     * than refusing an object that might just fit.
     *
     * @return the index into @p dirs, or nothing when not one of them can be written to at all.
     */
    inline std::optional<std::size_t> ChooseStorageDir(const std::vector<std::filesystem::path> &dirs, const std::size_t start,
                                                        const std::uintmax_t requiredBytes, const std::uintmax_t minFreeBytes,
                                                        const AvailableBytes &available) {
        if (dirs.empty()) return std::nullopt;

        std::optional<std::size_t> roomiest;
        std::uintmax_t roomiestBytes = 0;
        for (std::size_t i = 0; i < dirs.size(); ++i) {
            const auto index = (start + i) % dirs.size();
            const auto free = available(dirs[index]);
            if (!free.has_value()) continue;
            if (*free >= requiredBytes && *free - requiredBytes >= minFreeBytes) return index;
            if (!roomiest.has_value() || *free > roomiestBytes) {
                roomiest = index;
                roomiestBytes = *free;
            }
        }
        return roomiest;
    }

    /**
     * @brief The directory a new object was given, as recorded on it (Object::internalDir), and
     * the path its file is written under.
     */
    struct StorageDir {

        /**
         * @brief What goes into Object::internalDir: the configured path, or empty for the data
         * directory when no storage-dirs are configured - which is exactly what every object
         * written before this existed carries.
         */
        std::string internalDir;

        /**
         * @brief Where the file goes - ObjectRoot() of internalDir.
         */
        std::filesystem::path root;
    };

    /**
     * @brief The next directory to store a new object of @p requiredBytes in.
     *
     * @par
     * euclid.modules.esm.storage-dirs, an array of paths - typically one per filesystem - taken in
     * turn. Without it, or with it empty, the data directory as before, and the object records no
     * directory. Directories are created on first use. The rotation is per ESM process: with
     * several instances the spread is even over time rather than strictly alternating.
     *
     * @par
     * euclid.modules.esm.storage-min-free-mb (default 1024) is what a filesystem keeps free before
     * it is passed over - see ChooseStorageDir().
     *
     * @return nothing when no configured directory can be created or measured.
     */
    inline std::optional<StorageDir> NextStorageDir(const std::filesystem::path &dataDir, const std::uintmax_t requiredBytes) {

        const auto &configuration = Core::Configuration::instance();
        std::vector<std::string> configured;
        if (configuration.has("euclid.modules.esm.storage-dirs")) {
            try {
                configured = configuration.getArray<std::string>("euclid.modules.esm.storage-dirs");
            } catch (const std::exception &) {
                // Not an array of strings: read as unset rather than taking ESM down over it.
                configured.clear();
            }
        }
        std::erase_if(configured, [](const std::string &dir) { return dir.empty(); });

        if (configured.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dataDir, ec);
            if (ec) return std::nullopt;
            return StorageDir{.internalDir = {}, .root = dataDir};
        }

        const std::vector<std::filesystem::path> dirs(configured.begin(), configured.end());
        const auto minFreeMb = std::max<long>(0, configuration.getOr<long>("euclid.modules.esm.storage-min-free-mb", 1024));

        static std::atomic<std::size_t> next{0};
        const auto start = next.fetch_add(1, std::memory_order_relaxed);

        const auto chosen = ChooseStorageDir(dirs, start % dirs.size(), requiredBytes, static_cast<std::uintmax_t>(minFreeMb) * 1024 * 1024,
                                             [](const std::filesystem::path &dir) -> std::optional<std::uintmax_t> {
                                                 std::error_code ec;
                                                 std::filesystem::create_directories(dir, ec);
                                                 if (ec) return std::nullopt;
                                                 const auto space = std::filesystem::space(dir, ec);
                                                 if (ec) return std::nullopt;
                                                 return space.available;
                                             });
        if (!chosen.has_value()) return std::nullopt;
        return StorageDir{.internalDir = configured[*chosen], .root = dirs[*chosen]};
    }

}// namespace Euclid::ESM
