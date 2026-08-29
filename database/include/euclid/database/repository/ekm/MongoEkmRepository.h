//
// Created by vogje01 on 29/05/2023.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/ekm/Key.h>
#include <euclid/database/repository/ekm/IEkmRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief EKM MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEkmRepository final : public IEkmRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEkmRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEkmRepository &instance() {
            static MongoEkmRepository ekmDatabase;
            return ekmDatabase;
        }

        /**
         * @brief Update or insert a key
         *
         * @param key key entity
         */
        Entity::EKM::Key upsertKey(Entity::EKM::Key &key) override;

        /**
         * @brief Removes a queue entity
         *
         * @param name module name
         */
        // void removeQueueByName(const std::string &name) override;

        /**
         * @brief Removes a queue entity by ERN
         *
         * @param ern queue ERN
         */
        // void deleteQueueByErn(const std::string &ern) override;

        /**
         * @brief Find by queue name
         *
         * @param name queue name
         * @return optional queue
         */
        // [[nodiscard]]
        // std::optional<Entity::EQS::Queue> findQueueByName(const std::string &name) const override;

        /**
         * @brief Find by queue ID
         *
         * @param oid queue OID
         * @return optional queue
         */
        // [[nodiscard]]
        // std::optional<Entity::EQS::Queue> findQueueById(const std::string &oid) const override;

        /**
         * @brief Find by queue ERN
         *
         * @param ern queue ERN
         * @return optional queue
         */
        // [[nodiscard]]
        // std::optional<Entity::EQS::Queue> findQueueByErn(const std::string &ern) const override;

        /**
         * @brief Find all keys
         *
         * @param prefix only queues whose name starts with this prefix are returned; empty matches all queues
         * @param pageSize maximum number of queues to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "arn"); empty means unsorted
         * @return list of all queue entities.
         */
        [[nodiscard]]
        std::vector<Entity::EKM::Key> listKeys(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override;

        /**
         * @brief Check the existence of the module by name
         *
         * @param name module name check existence
         * @return true if module exists
         */
        // [[nodiscard]]
        // bool queueExists(const std::string &name) const override;

        /**
         * @brief Get the total number of keys
         *
         * @param accountId only keys belonging to this account are counted
         * @param nameSpace only keys in this namespace are counted; empty means don't filter by namespace
         * @return total number of keys
         */
        [[nodiscard]]
        long countKeys(const std::string &accountId, const std::string &nameSpace, const std::string &prefix = "") const override;

        /**
         * @brief Delete all modules
         */
        // void clearQueues() override;

        /**
         * @brief Update or insert a message
         *
         * @param message message entity
         */
        // void upsertMessage(const Entity::EQS::Message &message) override;

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
        // Entity::EQS::Message sendMessage(const std::string &messageId, const std::string &ern, const std::string &queueErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes, Entity::EQS::MessagePriority priority) override;

        /**
         * @brief Receives up to maxCount available messages from a queue, long-polling for up to waitTime seconds
         *
         * @param queueErn queue ERN
         * @param maxCount maximal number of messages to return
         * @param waitTime maximal number of seconds to wait for messages to become available
         * @return up to maxCount messages; empty if none became available within waitTime
         */
        // std::vector<Entity::EQS::Message> receiveMessages(const std::string &queueErn, long maxCount, long waitTime) override;

        /**
         * @brief Deletes a message entity
         *
         * @param receiptHandle receipt handle of the message
         */
        // void deleteMessage(const std::string &receiptHandle) override;

        /**
         * @brief Deletes all messages of a queue
         *
         * @param queueErn queue ERN
         */
        // void purgeQueue(const std::string &queueErn) override;

        /**
         * @brief Deletes all messages of every queue in a region/account
         *
         * @param region region of the queues to purge
         * @param accountId account ID of the queues to purge
         */
        // void purgeAllQueues(const std::string &region, const std::string &accountId) override;

    private:

        static constexpr auto DATABASE_NAME = "euclid";
        static constexpr auto KEY_COLLECTION = "ekm_key";

        /**
         * @brief Creates the indexes required for efficient message lookup, if they do not already exist.
         *
         * Without these, listKeys()/deleteKeys()/upsertKey() degrade to full collection
         * scans over sqs_message as it grows, which under concurrent load causes receives to stall.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database