//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::ESM {

    /**
     * @brief Lifecycle status of a storage object across a multipart upload.
     *
     * CREATED (create-upload done) -> UPLOADING (first upload-part received) -> UPLOADED (all
     * parts received, complete-upload called) -> COMPLETED (post-processing - assembling the
     * final file, computing its MD5 sum, determining its content type - has finished).
     *
     * @author jens.vogt\@opitz-consulting.com
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

}// namespace Euclid::Database::Entity::ESM
