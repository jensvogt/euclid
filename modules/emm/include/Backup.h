// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <string>
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

}// namespace Euclid::EMM
