// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EAM {

    /**
     * @brief Something shadow mode would have refused.
     *
     * @par
     * The whole reason shadow mode exists. Deny-by-default applied to an installation built the
     * other way round locks out everybody at once, and nobody can derive the grants a live system
     * needs from first principles - so the system is asked. Each of these is a grant somebody has
     * to write before enforcing, or a call that genuinely should stop working.
     *
     * @par
     * Recorded per *distinct* gap rather than per request: one row per
     * (user, account, namespace, target, action), with a count. A busy installation refused a
     * million times over a week has a handful of gaps and a large count, and it is the handful that
     * has to be read by a person.
     *
     * @par
     * Written by every module process and read by EAM, which is why it is a collection rather than
     * something held in memory - a gap seen by EQS has to be visible to `eam authorization-gaps`.
     *
     * @par
     * See docs/role-concept.md §5.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct AuthorizationGap {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief The user that would have been refused.
         */
        std::string userId;

        /**
         * @brief The account the request was acting in.
         */
        std::string accountId;

        /**
         * @brief The namespace it was scoped to, empty for the account root.
         */
        std::string nameSpace;

        /**
         * @brief Module target, e.g. "ens".
         */
        std::string target;

        /**
         * @brief Module action, e.g. "publish-message".
         */
        std::string action;

        /**
         * @brief Why it would have been refused, as Authorization::Allows() put it - a missing
         * grant reads differently from a grant that does not cover this action, and the two want
         * different fixes.
         */
        std::string reason;

        /**
         * @brief How many times this gap has been seen.
         */
        long count{};

        /**
         * @brief When it was first seen - how long shadow mode has been watching this one.
         */
        std::chrono::system_clock::time_point firstSeen;

        /**
         * @brief When it was last seen. A gap that stopped appearing after a grant was written is
         * one that has been fixed.
         */
        std::chrono::system_clock::time_point lastSeen;

        /**
         * @brief Converts the entity to a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts a MongoDB document to an entity.
         */
        static AuthorizationGap fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::EAM
