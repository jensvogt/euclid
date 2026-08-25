//
// Created by vogje01 on 12/10/23.
//

#pragma once

// Boost includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

namespace Euclid::Database::Entity::ENS {

    /**
     * @brief ENS topic attribute entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct TopicAttribute {

        /**
         * Delay seconds
         *
         * <p>The length of time, in seconds, for which the delivery of all resources in the queue is delayed. Valid values: An integer from 0 to 900 (15 minutes). Default: 0.<p>
         */
        long delaySeconds = 0;

        /**
         * Max message size (256kB).
         *
         * <p> The limit of how many bytes a message can contain before Amazon SQS rejects it. Valid values: An integer from 1,024 bytes (1 KiB) up to 262,144 bytes (256 KiB). Default: 262,144 (256 KiB).</p>
         */
        long maxMessageSize = 262144;

        /**
         * Max retention period (4d).
         *
         * <p>The length of time, in seconds, for which Amazon SQS retains a message. Valid values: An integer representing seconds, from 60 (1 minute) to 1,209,600 (14 days). Default: 345,600 (4 days). When you
         * change a queue's attributes, the change can take up to 60 seconds for most of the attributes to propagate throughout the Amazon SQS system. Changes made to the MessageRetentionPeriod attribute can take
         * up to 15 minutes and will impact existing resources in the queue potentially causing them to be expired and deleted if the MessageRetentionPeriod is reduced below the age of existing resources.</p>
         */
        long messageRetentionPeriod = 345600;

        /**
         * Receive message timeout.
         *
         * <p>The length of time, in seconds, for which a ReceiveMessage action waits for a message to arrive. Valid values: An integer from 0 to 20 (seconds). Default: 0.</p>
         */
        long receiveMessageWaitTime = 20;

        /**
         * Visibility timeout.
         *
         * <p>The visibility timeout for the queue, in seconds. Valid values: An integer from 0 to 43,200 (12 hours). Default: 30. For more information about the visibility timeout, see Visibility Timeout in the Amazon SQS Developer Guide.</p>
         */
        long visibilityTimeout = 30;

        /**
         * Policy
         *
         * <p>The queue's policy. A valid Euclid policy.
         */
        std::string policy;

        /**
         * Number of messages
         */
        long approximateNumberOfMessages = 0;

        /**
         * Delay counter
         */
        long approximateNumberOfMessagesDelayed = 0;

        /**
         * Not visible counter
         */
        long approximateNumberOfMessagesNotVisible = 0;

        /**
         * Euclid ARN
         */
        std::string queueErn;

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
         * @param document MongoDB document view.
         */
        [[maybe_unused]]
        void FromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::SQS