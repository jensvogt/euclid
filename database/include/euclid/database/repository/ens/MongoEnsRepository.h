//
// Created by vogje01 on 29/05/2023.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/ens/IEnsRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief ENS MongoDB database.
     *
     * Controls all the Euclid topicing modules.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEnsRepository final : public IEnsRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEnsRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEnsRepository &instance() {
            static MongoEnsRepository ensDatabase;
            return ensDatabase;
        }

        /**
         * @brief Update or insert a topic
         *
         * @param topic topic entity
         */
        Entity::ENS::Topic upsertTopic(Entity::ENS::Topic &topic) override;

        /**
         * @brief Creates or refreshes a subscription (upsert keyed on sourceErn/type/targetErn)
         *
         * @param subscription subscription entity
         */
        Entity::ENS::Subscription upsertSubscription(Entity::ENS::Subscription &subscription) override;

        /**
         * @brief Lists every subscription fanning out from a topic.
         *
         * @param sourceErn topic ERN
         */
        [[nodiscard]]
        std::vector<Entity::ENS::Subscription> listSubscriptionsBySourceErn(const std::string &sourceErn) const override;

        /**
         * @brief Deletes a subscription by its ERN.
         *
         * @param ern subscription ERN
         */
        void deleteSubscriptionByErn(const std::string &ern) override;

        /**
         * @brief Removes a topic entity
         *
         * @param name module name
         */
        //void removeTopicByName(const std::string &name) override;

        /**
         * @brief Removes a topic entity by ERN
         *
         * @param ern topic ERN
         */
        void deleteTopicByErn(const std::string &ern) override;

        /**
         * @brief Find by topic name
         *
         * @param name topic name
         * @return optional topic
         */
        [[nodiscard]]
        std::optional<Entity::ENS::Topic> findTopicByName(const std::string &name) const override;

        /**
         * @brief Find by topic ID
         *
         * @param oid topic OID
         * @return optional topic
         */
        // [[nodiscard]]
        // std::optional<Entity::EQS::Topic> findTopicById(const std::string &oid) const override;

        /**
         * @brief Find by topic ERN
         *
         * @param ern topic ERN
         * @return optional topic
         */
        [[nodiscard]]
        std::optional<Entity::ENS::Topic> findTopicByErn(const std::string &ern) const override;

        /**
         * @brief Find all topics
         *
         * @param accountId only topics belonging to this account are returned
         * @param namespaceName only topics in this namespace are returned; empty means don't filter by namespace
         * @param pageSize maximum number of topics to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "arn"); empty means unsorted
         * @return list of all topic entities.
         */
        [[nodiscard]]
        std::vector<Entity::ENS::Topic> listTopics(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        /**
         * @brief Check the existence of the module by name
         *
         * @param name module name check existence
         * @return true if module exists
         */
        [[nodiscard]]
        bool topicExists(const std::string &name) const override;

        /**
         * @brief Get the total number of modules
         *
         * @param accountId only topics belonging to this account are returned
         * @param namespaceName only topics in this namespace are returned; empty means don't filter by namespace
         * @return total number of modules
         */
        [[nodiscard]]
        long countTopics(const std::string &accountId, const std::string &namespaceName) const override;

        /**
         * @brief Delete all modules
         */
        // void clearTopics() override;

        /**
         * @brief Update or insert a message
         *
         * @param message message entity
         */
        void upsertMessage(const Entity::ENS::Message &message) override;

        /**
         * @brief Publish a message to a topic
         *
         * @param messageId messageId
         * @param ern message ERN
         * @param topicErn topic ERN
         * @param body message body
         * @param attributes message attributes
         * @return the newly created message entity
         */
        Entity::ENS::Message publishMessage(const std::string &messageId, const std::string &ern, const std::string &topicErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes) override;

        /**
         * @brief Receives up to maxCount available messages from a topic, long-polling for up to waitTime seconds
         *
         * @param topicErn topic ERN
         * @param maxCount maximal number of messages to return
         * @param waitTime maximal number of seconds to wait for messages to become available
         * @return up to maxCount messages; empty if none became available within waitTime
         */
        // std::vector<Entity::EQS::Message> receiveMessages(const std::string &topicErn, long maxCount, long waitTime) override;

        /**
         * @brief Deletes a message entity
         *
         * @param receiptHandle receipt handle of the message
         */
        // void deleteMessage(const std::string &receiptHandle) override;

        /**
         * @brief Deletes all messages of a topic
         *
         * @param topicErn topic ERN
         */
        void purgeTopic(const std::string &topicErn) override;

        /**
         * @brief Deletes all messages of every topic in a region/account/nameSpace
         *
         * @param region region of the topics to purge
         * @param accountId account ID of the topics to purge
         * @param nameSpace name space of the topics to purge
         */
        void purgeAllTopics(const std::string &region, const std::string &accountId, const std::string &nameSpace) override;

        /**
         * @brief Find by message name
         *
         * @param name message name
         * @return optional message
         */
        // [[nodiscard]]
        // std::optional<Entity::EQS::Message> findMessageByName(const std::string &name) const override;

        /**
         * @brief Find by message ID
         *
         * @param messageId message OID
         * @return optional message
         */
        [[nodiscard]]
        std::optional<Entity::ENS::Message> findMessageById(const std::string &messageId) const override;

        /**
         * @brief Find all messages
         *
         * @return list of all message entities.
         */
        // [[nodiscard]]
        // std::vector<Entity::EQS::Message> findAllMessages() const override;

        /**
         * @brief Lists a topic's messages without receiving them, paginated and sorted.
         *
         * @param topicErn ERN of the topic whose messages are listed.
         * @param pageSize maximum number of messages to return, or <= 0 for no limit.
         * @param pageIndex zero-based page index.
         * @param sortColumn message field to sort ascending by; empty leaves the order unspecified.
         * @return the requested page of messages.
         */
        [[nodiscard]]
        std::vector<Entity::ENS::Message> listMessages(const std::string &topicErn, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        /**
         * @brief Check the existence of the module by name
         *
         * @param messageId module name check existence
         * @return true if module exists
         */
        // [[nodiscard]]
        // bool messageExists(const std::string &messageId) const override;

        /**
         * @brief Get the total number of messages
         *
         * @return total number of messages
         */
        [[nodiscard]]
        long countMessages() const override;

        /**
         * @brief Get the total number of messages of a topic
         *
         * @param topicErn topic ERN
         * @return total number of messages of the topic
         */
        [[nodiscard]]
        long countMessages(const std::string &topicErn) const override;

        /**
         * @brief Delete all modules
         */
        // void clearMessages() override;

        /**
         * @brief Makes messages visible again once their visibility timeout has elapsed
         *
         * @return number of messages that were reset to status "INITIAL"
         */
        // long resetExpiredMessages() override;

    private:

        static constexpr auto DATABASE_NAME = "euclid";
        static constexpr auto TOPIC_COLLECTION = "ens_topic";
        static constexpr auto MESSAGE_COLLECTION = "ens_message";
        static constexpr auto SUBSCRIPTION_COLLECTION = "ens_subscription";

        /**
         * @brief Creates the indexes required for efficient message lookup, if they do not already exist.
         *
         * Without these, receiveMessages()/deleteMessage()/upsertMessage() degrade to full collection
         * scans over sqs_message as it grows, which under concurrent load causes receives to stall.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database