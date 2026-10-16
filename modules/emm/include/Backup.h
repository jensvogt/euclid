// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace Euclid::EMM {

    /**
     * @brief Prefix every backup archive's file name starts with.
     *
     * @par
     * What tells this module's archives apart from anything else an operator has put in the backup
     * directory - retention deletes files, and it deletes only what it recognises as its own.
     */
    inline constexpr auto kBackupPrefix = "euclid-backup-";

    /**
     * @brief Suffix every backup archive's file name ends with.
     */
    inline constexpr auto kBackupSuffix = ".zip";

    /**
     * @brief Whether a file name is one of this module's backup archives.
     *
     * @param name a file name, without its directory
     * @return true if retention may consider it
     */
    inline bool IsBackupArchive(const std::string &name) {
        return name.starts_with(kBackupPrefix) && name.ends_with(kBackupSuffix) &&
               name.size() > std::string(kBackupPrefix).size() + std::string(kBackupSuffix).size();
    }

    /**
     * @brief Which archives retention should delete, given everything in the backup directory.
     *
     * @par
     * A backup that runs every night and is never pruned fills the disk it exists to protect, and
     * does it quietly - the failure where the backup takes the installation down. So the newest
     * `keep` archives stay and the rest go.
     *
     * @par
     * Archives are named with an ISO timestamp, so sorting by name sorts by age. That is the only
     * reason this can decide anything from names alone, and it is why the name format is not free
     * to change without changing this.
     *
     * @par
     * `keep` of zero or less disables retention rather than deleting everything: a misread or
     * missing setting should leave backups alone, not destroy them all. Anything the directory
     * holds that is not one of this module's archives is never returned, whatever it is called.
     *
     * @param names file names in the backup directory, in any order
     * @param keep how many of the newest archives to keep
     * @return the names to delete, oldest first
     */
    inline std::vector<std::string> BackupsToRemove(std::vector<std::string> names, const long keep) {

        if (keep <= 0) return {};

        std::erase_if(names, [](const std::string &name) { return !IsBackupArchive(name); });
        if (static_cast<long>(names.size()) <= keep) return {};

        std::ranges::sort(names);
        names.resize(names.size() - static_cast<std::size_t>(keep));
        return names;
    }

    /**
     * @brief Suffix of the uncompressed payload a backup writes before zipping it.
     *
     * @par
     * Deleted once the archive exists. One still lying in the directory is a backup that is being
     * written right now, or one that died part-way - killed for memory, say, which no log line
     * survives to tell.
     */
    inline constexpr auto kBackupPayloadSuffix = ".json";

    /**
     * @brief When a backup started, read from its file name.
     *
     * @par
     * "euclid-backup-2026-10-08T01-00-00.zip" gives "2026-10-08T01:00:00Z" - the UTC time the
     * name was stamped with, ':' having been replaced by '-' to make it a valid file name.
     *
     * @param name an archive or payload file name, without its directory
     * @return the ISO 8601 timestamp, or an empty string if the name does not carry one
     */
    inline std::string BackupStartedAt(const std::string &name) {
        const std::string prefix(kBackupPrefix);
        if (!name.starts_with(prefix)) return {};
        // yyyy-mm-ddThh-mm-ss
        constexpr std::size_t kStampLength = 19;
        if (name.size() < prefix.size() + kStampLength) return {};
        auto stamp = name.substr(prefix.size(), kStampLength);
        if (stamp[4] != '-' || stamp[7] != '-' || stamp[10] != 'T' || stamp[13] != '-' || stamp[16] != '-') return {};
        for (const std::size_t i: {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U}) {
            if (stamp[i] < '0' || stamp[i] > '9') return {};
        }
        stamp[13] = ':';
        stamp[16] = ':';
        return stamp + "Z";
    }

    /**
     * @brief One file in the backup directory that belongs to this module.
     */
    struct BackupFile {

        /**
         * @brief File name, without its directory.
         */
        std::string name;

        /**
         * @brief When the backup started, from the name - see BackupStartedAt().
         */
        std::string startedAt;

        /**
         * @brief Size on disk, in bytes.
         */
        std::uintmax_t size{};

        /**
         * @brief When the file was last written.
         */
        std::chrono::system_clock::time_point modified;

        /**
         * @brief True for a finished archive, false for a payload left behind by a backup that is
         * still running or never finished.
         */
        bool complete{};
    };

    /**
     * @brief The backups in a directory, newest first.
     *
     * @par
     * Finished archives and leftover payloads alike, so a backup that dies every night shows up as
     * what it is rather than as a gap. Anything else in the directory is not listed.
     *
     * @param directory the backup directory
     * @param ec        set if the directory cannot be read; a missing directory is not an error
     *                  but an empty list - no backup has run yet
     * @return the backups, newest first
     */
    inline std::vector<BackupFile> ListBackups(const std::filesystem::path &directory, std::error_code &ec) {

        ec.clear();
        std::vector<BackupFile> result;
        if (!std::filesystem::exists(directory, ec)) return result;

        for (const auto &entry: std::filesystem::directory_iterator(directory, ec)) {
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc)) continue;

            auto name = entry.path().filename().string();
            const bool archive = IsBackupArchive(name);
            const bool payload = name.starts_with(kBackupPrefix) && name.ends_with(kBackupPayloadSuffix);
            if (!archive && !payload) continue;

            BackupFile file;
            file.startedAt = BackupStartedAt(name);
            file.size = entry.file_size(entryEc);
            if (entryEc) file.size = 0;
            // file_clock to system_clock without clock_cast, which libc++ does not have everywhere.
            if (const auto written = entry.last_write_time(entryEc); !entryEc) {
                file.modified = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        std::chrono::system_clock::now() + (written - std::filesystem::file_time_type::clock::now()));
            }
            file.complete = archive;
            file.name = std::move(name);
            result.push_back(std::move(file));
        }
        if (ec) return {};

        // Names sort by their timestamp (and at the same one, ".zip" before ".json").
        std::ranges::sort(result, [](const BackupFile &a, const BackupFile &b) { return a.name > b.name; });
        return result;
    }

}// namespace Euclid::EMM
