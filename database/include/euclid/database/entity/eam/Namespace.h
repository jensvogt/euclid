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
     * @brief A namespace (e.g. development/integration/production) scoped under an Account.
     * Namespace names are only unique within their account - the natural key is the compound
     * (accountId, name) pair, not name alone.
     */
    struct Namespace {

        /**
         * ID
         */
        std::string oid;

        /**
         * @brief Account this namespace belongs to. Part of the compound natural key.
         */
        std::string accountId;

        /**
         * @brief Namespace name, unique within its account. Part of the compound natural key.
         */
        std::string name;

        /**
         * @brief Euclid resource name, e.g. "ern:euclid:eam:eu-central-1:<accountId>:namespace:<name>"
         * (see Core::createEamNamespaceErn()).
         */
        std::string ern;

        /**
         * @brief Free-text description of the namespace's purpose.
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
        static Namespace fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}
