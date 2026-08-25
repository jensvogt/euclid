#pragma once

// C++ includes
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EAM {

    /**
     * @brief A tenant account. Namespaces and their resources are scoped under an account, and
     * users are granted access to specific (account, namespace) pairs - see User::accountGrants.
     */
    struct Account {

        /**
         * ID
         */
        std::string oid;

        /**
         * @brief Account ID, unique across the deployment. This is the same accountId already
         * carried on User/UserGroup and other resources.
         */
        std::string accountId;

        /**
         * @brief Human-readable account name.
         */
        std::string name;

        /**
         * @brief Euclid resource name, e.g. "ern:euclid:eam:eu-central-1:<accountId>:account:<accountId>"
         * (see Core::createEamAccountErn()).
         */
        std::string ern;

        /**
         * @brief Free-text description of the account's purpose.
         */
        std::string description;

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::chrono::system_clock::time_point modified;

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        static Account fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}
