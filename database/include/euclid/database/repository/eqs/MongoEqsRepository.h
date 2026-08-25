//
// Created by vogje01 on 29/05/2023.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/emm/Module.h>
#include <euclid/database/entity/eqs/PriorityWeights.h>
#include <euclid/database/repository/eqs/IEqsRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief Queueing MongoDB database.
     *
     * Controls all the Euclid queueing modules.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEqsRepository final : public IEqsRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEqsRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEqsRepository &instance() {
            static MongoEqsRepository eqsDatabase;
            return eqsDatabase;
        }

        /**
         * @brief Update or insert a queue
         *
         * @param queue queue entity
         */
        Entity::EQS::Queue upsertQueue(Entity::EQS::Queue &queue) override;

        /**
         * @brief Removes a queue entity
         *
         * @param name module name
         */
        void removeQueueByName(const std::string &name) override;

        /**
         * @brief Removes a queue entity by ERN
         *
         * @param ern queue ERN
         */
        void deleteQueueByErn(const std::string &ern) override;

        /**
         * @brief Find by queue name
         *
         * @param name queue name
         * @return optional queue
         */
        [[nodiscard]]
        std::optional<Entity::EQS::Queue> findQueueByName(const std::string &name) const override;

        /**
         * @brief Find by queue ID
         *
         * @param oid queue OID
         * @return optional queue
         */
        [[nodiscard]]
        std::optional<Entity::EQS::Queue> findQueueById(const std::string &oid) const override;

        /**
         * @brief Find by queue ERN
         *
         * @param ern queue ERN
         * @return optional queue
         */
        [[nodiscard]]
        std::optional<Entity::EQS::Queue> findQueueByErn(const std::string &ern) const override;

        /**
         * @brief Find all queues
         *
         * @param prefix only queues whose name starts with this prefix are returned; empty matches all queues
         * @param pageSize maximum number of queues to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "arn"); empty means unsorted
         * @return list of all queue entities.
         */
        [[nodiscard]]
        std::vector<Entity::EQS::Queue> listQueues(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        /**
         * @brief Check the existence of the module by name
         *
         * @param name module name check existence
         * @return true if module exists
         */
        [[nodiscard]]
        bool queueExists(const std::string &name) const override;

        /**
         * @brief Get the total number of modules
         *
         * @return total number of modules
         */
        [[nodiscard]]
        long countQueues() const override;

        /**
         * @brief Delete all modules
         */
        void clearQueues() override;

        /**
         * @brief Update or insert a message
         *
         * @param message message entity
         */
        void upsertMessage(const Entity::EQS::Message &message) override;

        /**
         * @brief Sends a message to a queue
         *
         * @param messageId messageId
         * @param ern message ERN
         * @param queueErn queue ERN
         * @param body message body
         * @param attributes message attributes
         * @param priority message priority; defaults to MIDDLE
         * @return the newly created message entity
         */
        Entity::EQS::Message sendMessage(const std::string &messageId, const std::string &ern, const std::string &queueErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes, Entity::EQS::MessagePriority priority) override;

        /**
         * @brief Receives up to maxCount available messages from a queue, long-polling for up to waitTime seconds
         *
         * @param queueErn queue ERN
         * @param maxCount maximal number of messages to return
         * @param waitTime maximal number of seconds to wait for messages to become available
         * @return up to maxCount messages; empty if none became available within waitTime
         */
        std::vector<Entity::EQS::Message> receiveMessages(const std::string &queueErn, long maxCount, long waitTime) override;

        /**
         * @brief Deletes a message entity
         *
         * @param receiptHandle receipt handle of the message
         */
        void deleteMessage(const std::string &receiptHandle) override;

        /**
         * @brief Deletes all messages of a queue
         *
         * @param queueErn queue ERN
         */
        void purgeQueue(const std::string &queueErn) override;

        /**
         * @brief Deletes all messages of every queue in a region/account
         *
         * @param region region of the queues to purge
         * @param accountId account ID of the queues to purge
         */
        void purgeAllQueues(const std::string &region, const std::string &accountId) override;

        /**
         * @brief Find by message name
         *
         * @param name message name
         * @return optional message
         */
        [[nodiscard]]
        std::optional<Entity::EQS::Message> findMessageByName(const std::string &name) const override;

        /**
         * @brief Find by message ID
         *
         * @param oid message OID
         * @return optional message
         */
        [[nodiscard]]
        std::optional<Entity::EQS::Message> findMessageById(const std::string &oid) const override;

        /**
         * @brief Find all messages
         *
         * @return list of all message entities.
         */
        [[nodiscard]]
        std::vector<Entity::EQS::Message> findAllMessages() const override;

        /**
         * @brief Lists a queue's messages without receiving them, paginated and sorted.
         *
         * @param queueErn ERN of the queue whose messages are listed.
         * @param pageSize maximum number of messages to return, or <= 0 for no limit.
         * @param pageIndex zero-based page index.
         * @param sortColumn message field to sort ascending by; empty leaves the order unspecified.
         * @return the requested page of messages.
         */
        [[nodiscard]]
        std::vector<Entity::EQS::Message> listMessages(const std::string &queueErn, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        /**
         * @brief Check the existence of the module by name
         *
         * @param messageId module name check existence
         * @return true if module exists
         */
        [[nodiscard]]
        bool messageExists(const std::string &messageId) const override;

        /**
         * @brief Get the total number of modules
         *
         * @return total number of modules
         */
        [[nodiscard]]
        long countMessages() const override;

        /**
         * @brief Get the total number of messages of a queue
         *
         * @param queueErn queue ERN
         * @return total number of messages of the queue
         */
        [[nodiscard]]
        long countMessages(const std::string &queueErn) const override;

        /**
         * @brief Delete all modules
         */
        void clearMessages() override;

        /**
         * @brief Makes messages visible again once their visibility timeout has elapsed
         *
         * @return number of messages that were reset to status "INITIAL"
         */
        long resetExpiredMessages() override;

    private:

        static constexpr auto DATABASE_NAME = "euclid";
        static constexpr auto QUEUE_COLLECTION = "eqs_queue";
        static constexpr auto MESSAGE_COLLECTION = "eqs_message";

        /**
         * @brief Creates the indexes required for efficient message lookup, if they do not already exist.
         *
         * Without these, receiveMessages()/deleteMessage()/upsertMessage() degrade to full collection
         * scans over sqs_message as it grows, which under concurrent load causes receives to stall.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database