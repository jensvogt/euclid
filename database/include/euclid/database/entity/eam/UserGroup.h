#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EAM {

    /**
     * @brief A named group of users. A user can belong to any number of groups, and a group
     * starts out empty - membership is managed separately from group creation.
     */
    struct UserGroup {

        /**
         * ID
         */
        std::string oid;

        /**
         * @brief Group name, unique across the deployment.
         */
        std::string name;

        /**
         * @brief Euclid resource name, e.g. "ern:euclid:access:eu-central-1:<accountId>:usergroup:<name>"
         * (see Core::createAccessUserGroupErn()).
         */
        std::string ern;

        /**
         * @brief Account the group belongs to.
         */
        std::string accountId;

        /**
         * @brief Region the group was created in.
         */
        std::string region;

        /**
         * @brief Free-text description of the group's purpose.
         */
        std::string description;

        /**
         * @brief User IDs currently in this group. Empty on creation.
         */
        std::vector<std::string> userIds;

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string createdAt;

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
        static UserGroup fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}
