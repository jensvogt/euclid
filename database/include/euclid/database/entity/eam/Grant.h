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
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EAM {

    /**
     * @brief One role, given to one principal, somewhere.
     *
     * @par
     * The only thing that grants anything, and the only thing that carries scope. A user's rights
     * are the union of the grants held by that user and by each group they belong to - there is no
     * other source, no precedence and no deny, so "what may this user do" is answered by reading
     * rather than by evaluating.
     *
     * @par
     * Replaces both of the fields authorization used to be spread across: User::accountGrants
     * becomes a grant scoped to an account and its namespaces, and User::resourceGrants becomes the
     * resources list here - attached to the grant rather than to the user, so "this application may
     * put objects into these two buckets" is one record instead of a permission set and a resource
     * list that have to be kept in step by hand.
     *
     * @par
     * See docs/role-concept.md §3.3.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Grant {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief The role being granted, by name.
         *
         * @par
         * A name rather than an ERN because a built-in role has no stored record to point at, and a
         * grant should be able to name one. Resolved against accountId: a stored role of that
         * account first, then the built-ins.
         */
        std::string role;

        /**
         * @brief Who it is granted to: a user ERN or a user-group ERN.
         *
         * @par
         * One field for both kinds, because the ERN says which - and because every place that reads
         * this wants "the grants that apply to this caller", which is both.
         */
        std::string principal;

        /**
         * @brief Account the grant applies in. A grant is scoped to exactly one.
         */
        std::string accountId;

        /**
         * @brief Namespaces of that account the grant applies in.
         *
         * @par
         * A single "*" means every namespace of the account, including the account root. An empty
         * list means none, which is a grant that grants nothing - refused when the grant is
         * created rather than stored as a silent no-op.
         */
        std::vector<std::string> namespaces;

        /**
         * @brief ERN patterns the grant applies to, for the actions that name a resource.
         *
         * @par
         * A single "*" means every resource. Otherwise each entry is an ERN that may end in "*",
         * e.g. "ern:ens:eu-central-1:000000000000:production:topic:order-*". Actions that name no
         * resource are unaffected - see docs/role-concept.md §4.1 for which do.
         */
        std::vector<std::string> resources;

        /**
         * @brief When it was granted.
         */
        std::chrono::system_clock::time_point granted;

        /**
         * @brief The user ID that granted it, for the audit question this model exists to answer.
         */
        std::string grantedBy;

        /**
         * @brief Converts the entity to a MongoDB document.
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts a MongoDB document to an entity.
         *
         * @param document MongoDB document.
         */
        static Grant fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::EAM
