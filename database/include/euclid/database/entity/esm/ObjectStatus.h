// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <optional>
#include <string>

namespace Euclid::Database::Entity::ESM {

    /**
     * @brief Lifecycle status of a storage object across a multipart upload.
     *
     * CREATED (create-upload done) -> UPLOADING (first upload-part received) -> UPLOADED (all
     * parts received, complete-upload called) -> COMPLETED (post-processing - assembling the
     * final file, computing its MD5 sum, determining its content type - has finished).
     *
     * @author jensvogt47\@gmail.com
     */
    enum class ObjectStatus {
        CREATED,
        UPLOADING,
        UPLOADED,
        COMPLETED,
        UNKNOWN
    };

    static std::map<ObjectStatus, std::string> ObjectStatusNames{
            {ObjectStatus::CREATED, "CREATED"},
            {ObjectStatus::UPLOADING, "UPLOADING"},
            {ObjectStatus::UPLOADED, "UPLOADED"},
            {ObjectStatus::COMPLETED, "COMPLETED"},
            {ObjectStatus::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string ObjectStatusToString(const ObjectStatus &objectStatus) {
        return ObjectStatusNames[objectStatus];
    }

    [[maybe_unused]]
    static ObjectStatus ObjectStatusFromString(const std::string &objectStatus) {
        const auto it = std::ranges::find_if(ObjectStatusNames, [&objectStatus](const auto &pair) { return pair.second == objectStatus; });
        return it != ObjectStatusNames.end() ? it->first : ObjectStatus::UNKNOWN;
    }

    /**
     * @brief Whether an object in this status can be handed to a reader.
     *
     * @par
     * Only a COMPLETED object has a file that is finished, hashed and named by its own
     * internalName. Everything else names a file that is still being written, or none at all.
     *
     * @param objectStatus the object's status.
     * @return true when get-object and download may serve it.
     */
    [[maybe_unused]]
    static bool IsDownloadable(const ObjectStatus &objectStatus) {
        return objectStatus == ObjectStatus::COMPLETED;
    }

    /**
     * @brief The status an object row carries once an upload to its key has been created.
     *
     * @par
     * A key nobody has written yet starts at CREATED: there is nothing to read, and a reader
     * should be told so. A key that already holds an object keeps whatever status it had, because
     * the object that is there stays readable until complete-upload replaces it - an upload is
     * something happening to the key, not something that happens to the version already published
     * under it.
     *
     * @par
     * Getting this wrong is not a small window. An upload that is never completed - a splitter
     * stopped mid-run, a crashed client - leaves the key at whatever this returns forever, so
     * returning CREATED here retires a perfectly good object on the strength of a write that
     * never arrived.
     *
     * @param existing the status of the object already at this key, or nothing for a new key.
     * @return the status to seed the row with.
     */
    [[maybe_unused]]
    static ObjectStatus StatusForCreatedUpload(const std::optional<ObjectStatus> &existing) {
        return existing.value_or(ObjectStatus::CREATED);
    }

}// namespace Euclid::Database::Entity::ESM
