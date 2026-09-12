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
     * @brief A named set of permissions, belonging to one account.
     *
     * @par
     * A role says *what* may be done and nothing about where or to which resource - that is the
     * grant's job (Entity::EAM::Grant). Keeping the two apart is what lets one role be bound in
     * two namespaces on different resources without being written twice.
     *
     * @par
     * Roles are per account: a role belongs to exactly one, and a grant can only name a role of the
     * account it grants in. Two accounts may each have a "topic-publisher" meaning different
     * things, the same way each may have a topic called "orders".
     *
     * @par
     * The built-in roles (Core::BuiltinRoles) are the exception and are never stored - they are
     * computed, referenced by name from any account, and cannot be created, changed or deleted
     * here. A stored role may not take one of their names.
     *
     * @par
     * See docs/role-concept.md.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Role {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Role name, unique within its account.
         */
        std::string name;

        /**
         * @brief Euclid resource name, e.g.
         * "ern:euclid:eam:eu-central-1:<accountId>::role/<name>".
         */
        std::string ern;

        /**
         * @brief Account this role belongs to.
         */
        std::string accountId;

        /**
         * @brief Region the role was created in.
         */
        std::string region;

        /**
         * @brief Free-text description of what the role is for.
         */
        std::string description;

        /**
         * @brief What the role grants, as "<module>:<action>" or "<module>:*" or "*:*".
         *
         * @par
         * Every entry is checked against Core::Permissions when the role is created or updated, so
         * a stored role cannot name an action no module dispatches - a permission that granted
         * nothing would be worse than being refused, because nobody would notice.
         */
        std::vector<std::string> permissions;

        /**
         * @brief Creation timestamp.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Last modification timestamp.
         */
        std::chrono::system_clock::time_point modified;

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
        static Role fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::EAM
