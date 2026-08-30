//
// Created by vogje01 on 8/30/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::EKM {

    /**
     * @brief Lifecycle status of an EKM key.
     *
     * AVAILABLE (default) -> REVOKED (revoke-key: encryption blocked, decryption still allowed)
     * or -> PENDING_DELETION (delete-key: encryption blocked, decryption still allowed until the
     * background purge sweep permanently deletes the key once its deletionDate passes).
     * REVOKED can still transition to PENDING_DELETION (revoke now, delete later); the reverse
     * (un-scheduling a deletion back to REVOKED) is deliberately not offered, to avoid a key that
     * still shows a deletionDate silently losing the status that explains it.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class KeyStatus {
        AVAILABLE,
        REVOKED,
        PENDING_DELETION,
        UNKNOWN
    };

    static std::map<KeyStatus, std::string> KeyStatusNames{
            {KeyStatus::AVAILABLE, "AVAILABLE"},
            {KeyStatus::REVOKED, "REVOKED"},
            {KeyStatus::PENDING_DELETION, "PENDING_DELETION"},
            {KeyStatus::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string KeyStatusToString(const KeyStatus &keyStatus) {
        return KeyStatusNames[keyStatus];
    }

    [[maybe_unused]]
    static KeyStatus KeyStatusFromString(const std::string &keyStatus) {
        const auto it = std::ranges::find_if(KeyStatusNames, [&keyStatus](const auto &pair) { return pair.second == keyStatus; });
        return it != KeyStatusNames.end() ? it->first : KeyStatus::UNKNOWN;
    }

}// namespace Euclid::Database::Entity::EKM
