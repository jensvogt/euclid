// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/UuidUtils.h>
#include <euclid/database/entity/esm/Object.h>

namespace Euclid::Database::Entity::ESM {

    /**
     * @brief What an upload that has finished does to the key it was made to.
     */
    enum class UploadOutcome {
        /**
         * @brief The new object takes the key and the one it replaces goes, file and all. Every
         * upload to a bucket without versioning, and a first upload to a key in one with it.
         */
        REPLACE,

        /**
         * @brief The new object takes the key and the one it replaces is kept as a non-current
         * version: its row goes to the version collection and its file stays.
         */
        NEW_VERSION,

        /**
         * @brief The bytes are the ones the key already holds. Nothing changes: the new file is
         * discarded, the current object stays as it is, and no event is published.
         */
        UNCHANGED
    };

    /**
     * @brief Decides what a finished upload does to its key.
     *
     * @par
     * Only a COMPLETED object is something to keep or to compare with. A key whose current row is
     * still CREATED or UPLOADING is a first upload that has not finished, and there is no earlier
     * version of it to preserve - so the new one replaces it, as it would in a bucket without
     * versioning.
     *
     * @param versioning whether the bucket has versioning on
     * @param existing the key's current object before this upload, if it had one
     * @param md5Sum MD5 of the bytes just uploaded
     */
    [[nodiscard]] inline UploadOutcome DecideUploadOutcome(const bool versioning, const std::optional<Object> &existing, const std::string &md5Sum) {
        if (!versioning || !existing.has_value()) return UploadOutcome::REPLACE;
        if (existing->status != ObjectStatus::COMPLETED || existing->md5Sum.empty()) return UploadOutcome::REPLACE;
        return existing->md5Sum == md5Sum ? UploadOutcome::UNCHANGED : UploadOutcome::NEW_VERSION;
    }

    /**
     * @brief The non-current versions a key no longer keeps under its bucket's limit.
     *
     * @param versions the key's non-current versions, newest first - as listObjectVersions() gives them
     * @param maxVersions the bucket's limit; 0 or less keeps them all
     * @return the oldest versions beyond the limit, in the order given
     */
    [[nodiscard]] inline std::vector<Object> VersionsBeyondLimit(const std::vector<Object> &versions, const std::int64_t maxVersions) {
        if (maxVersions <= 0 || static_cast<std::int64_t>(versions.size()) <= maxVersions) return {};
        return {versions.begin() + maxVersions, versions.end()};
    }

    /**
     * @brief A new version ID.
     */
    [[nodiscard]] inline std::string NewVersionId() {
        return Core::UuidUtils::CreateRandomUuid();
    }

    /**
     * @brief The row a superseded object is kept as: the object as it was, with a version ID of its
     * own - one written before versioning was turned on has none until now.
     */
    [[nodiscard]] inline Object SupersededVersion(const Object &current) {
        Object version = current;
        if (version.versionId.empty()) version.versionId = NewVersionId();
        version.oid.clear();
        return version;
    }

}// namespace Euclid::Database::Entity::ESM
