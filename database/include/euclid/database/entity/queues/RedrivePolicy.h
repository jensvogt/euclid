//
// Created by vogje01 on 12/10/23.
//

#pragma once

// C++ includes
#include <string>
#include <optional>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes

namespace Euclid::Database::Entity::Queues {

    /**
     * @brief SQS queue re-drive policy entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct RedrivePolicy {

        /**
         * Dead letter queue target ARN
         */
        std::string deadLetterTargetArn;

        /**
         * Maximal number of retries before the message will be sent to the DQL
         */
        long maxReceiveCount = 3;

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value ToDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        [[maybe_unused]] void FromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::SQS