//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <map>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/com/Variant.h>
#include <euclid/database/entity/ens/Message.h>
#include <euclid/database/entity/ens/Subscription.h>
#include <euclid/database/entity/ens/Topic.h>

namespace Euclid::Database {

    /**
     * @brief Interface for ENS repository operations.
     *
     * Provides an abstraction for storing, retrieving, and managing
     * ENS-related data.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class IEnsRepository {

    public:

        /**
         * @brief Virtual destructor for the IEnsRepository interface.
         *
         * Ensures derived classes' destructor is invoked correctly
         * during object destruction to release resources.
         */
        virtual ~IEnsRepository() = default;

        /**
         * @brief Inserts a new topic or updates an existing one in the repository.
         *
         * If a module with the same identifier already exists, its data will be updated
         * with the provided queue information. Otherwise, a new queue will be added
         * to the repository.
         *
         * @param topic The topic to be inserted or updated in the repository.
         */
        virtual Entity::ENS::Topic upsertTopic(Entity::ENS::Topic &topic) = 0;

        /**
         * @brief Creates (or, for a matching existing sourceErn/type/targetErn, refreshes) a
         * subscription that fans out messages published to sourceErn to targetErn.
         *
         * @param subscription the subscription to be inserted or updated in the repository.
         */
        virtual Entity::ENS::Subscription upsertSubscription(Entity::ENS::Subscription &subscription) = 0;

        /**
         * @brief Lists every subscription fanning out from a topic.
         *
         * @param sourceErn ERN of the topic whose subscriptions are listed.
         * @return the topic's subscriptions; empty if it has none.
         */
        [[nodiscard]]
        virtual std::vector<Entity::ENS::Subscription> listSubscriptionsBySourceErn(const std::string &sourceErn) const = 0;

        /**
         * @brief Deletes a subscription by its ERN. Deleting an ERN with no matching subscription
         * is not an error.
         *
         * @param ern subscription ERN
         */
        virtual void deleteSubscriptionByErn(const std::string &ern) = 0;

        /**
         * @brief Removes the specified element or elements from the collection or data structure.
         *
         * @param name The name of the queue to be removed.
         */
        // virtual void removeQueueByName(const std::string &name) = 0;

        /**
         * @brief Removes the specified element or elements from the collection or data structure by ERN.
         *
         * @param ern The Euclid resource name (ERN) of the topic to be removed.
         */
        virtual void deleteTopicByErn(const std::string &ern) = 0;

        /**
         * @brief Searches for a topic by its name.
         *
         * @param name The name of the topic to search for.
         * @return The item matching the given name, or nullptr if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ENS::Topic> findTopicByName(const std::string &name) const = 0;

        /**
         * @brief Locates a queue in the repository by its unique identifier.
         *
         * Searches for a queue matching the specified identifier and returns it
         * if found. If no matching module exists, an empty optional is returned.
         *
         * @param oid The unique identifier of the queue to search for.
         * @return An optional containing the found queue, or an empty optional
         *         if no module with the given identifier exists.
         */
        // [[nodiscard]]
        // virtual std::optional<Entity::EQS::Queue> findQueueById(const std::string &oid) const = 0;

        /**
         * @brief Searches for a topic by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the topic to search for.
         * @return The item matching the given ERN, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ENS::Topic> findTopicByErn(const std::string &ern) const = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @param accountId only queues belonging to this account are returned
         * @param namespaceName only queues in this namespace are returned; empty means don't filter by namespace
         * @param prefix only queues whose name starts with this prefix are returned; empty matches all queues
         * @param pageSize maximum number of queues to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "arn"); empty means unsorted
         * @param sortDirection "asc" or "desc"; anything else is treated as "desc"
         * @return A collection containing all entities or objects found.
         */
        [[nodiscard]]
        virtual std::vector<Entity::ENS::Topic> listTopics(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

        /**
         * @brief Checks if a topic with the specified name exists in the repository.
         *
         * @param name The name of the topic to check for existence.
         * @return True if a topic with the given name exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool topicExists(const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of topics in the repository.
         *
         * @param accountId only topics belonging to this account are counted
         * @param namespaceName only topics in this namespace are counted; empty means don't filter by namespace
         * @param prefix only topics whose name starts with this prefix are counted; empty means don't filter by name
         * @return The total number of topics as a long integer.
         */
        [[nodiscard]]
        virtual long countTopics(const std::string &accountId, const std::string &namespaceName, const std::string &prefix = "") const = 0;

        /**
         * @brief Removes all entries from the queue repository, leaving it in an empty state.
         *
         * This method is intended to clear all stored data, and the repository will contain no entities after its execution.
         *
         * This is a pure virtual function and must be implemented by derived classes.
         */
        // virtual void clearQueues() = 0;

        /**
         * @brief Inserts a new message or updates an existing one in the repository.
         *
         * If a message with the same identifier already exists, its data will be updated
         * with the provided message information. Otherwise, a new message will be added
         * to the repository.
         *
         * @param message The message to be inserted or updated in the repository.
         */
        virtual void upsertMessage(const Entity::ENS::Message &message) = 0;

        /**
         * @brief Publish a message to a topic.
         *
         * Builds a new message entity for the topic identified by its ERN, assigns it
         * a message ID, persists it in the repository, and returns the persisted entity.
         *
         * @param messageId message ID
         * @param ern ERN of the message
         * @param topicErn ERN of the topic the message is sent to.
         * @param body message body.
         * @param attributes message attributes.
         * @return the newly created message entity.
         */
        virtual Entity::ENS::Message publishMessage(const std::string &messageId, const std::string &ern, const std::string &topicErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes) = 0;

        /**
         * @brief Deletes ENS topic messages from the repository.
         *
         * @param topicErn The topic ERN of the messages to delete.
         */
        // virtual void deleteMessages(const std::string &topicErn) = 0;

        /**
         * @brief Deletes all messages of a topic.
         *
         * @param topicErn The Euclid resource name (ERN) of the topic whose messages are to be purged.
         */
        virtual void purgeTopic(const std::string &topicErn) = 0;

        /**
         * @brief Deletes all messages of every topics in a region/account/nameSpace.
         *
         * @param region region of the topics to purge.
         * @param accountId account ID of the topics to purge.
         * @param nameSpace name space of the topics to purge.
         */
        virtual void purgeAllTopics(const std::string &region, const std::string &accountId, const std::string &nameSpace) = 0;

        /**
         * @brief Searches for a message by its name.
         *
         * @param name The name of the message to search for.
         * @return The item matching the given name, or nullptr if no match is found.
         */
        // [[nodiscard]]
        // virtual std::optional<Entity::EQS::Message> findMessageByName(const std::string &name) const = 0;

        /**
         * @brief Locates a message in the repository by its message ID.
         *
         * Searches for a message matching the specified identifier and returns it
         * if found. If no matching module exists, an empty optional is returned.
         *
         * @param messageId The message ID of the message to search for.
         * @return An optional containing the found message, or an empty optional
         *         if no module with the given identifier exists.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ENS::Message> findMessageById(const std::string &messageId) const = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @return A collection containing all entities or objects found.
         */
        // [[nodiscard]]
        // virtual std::vector<Entity::EQS::Message> findAllMessages() const = 0;

        /**
         * @brief Lists the messages of a topic, without receiving them (i.e. without changing
         * their status or visibility), paginated and sorted.
         *
         * @param topicErn ERN of the topic whose messages are listed.
         * @param pageSize maximum number of messages to return, or <= 0 for no limit.
         * @param pageIndex zero-based page index, combined with pageSize to compute the offset.
         * @param sortColumn message field to sort ascending by, e.g. "created", "size",
         * "messageId"; unrecognized/empty leaves the order unspecified.
         * @param sortDirection "asc" or "desc"; anything else is treated as "desc"
         * @return the requested page of messages.
         */
        [[nodiscard]]
        virtual std::vector<Entity::ENS::Message> listMessages(const std::string &topicErn, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

        /**
         * @brief Checks if a message with the specified name exists in the repository.
         *
         * @param name The name of the message to check for existence.
         * @return True if a message with the given name exists, otherwise false.
         */
        // [[nodiscard]]
        // virtual bool messageExists(const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of messages in the repository.
         *
         * @return The total number of messages as a long integer.
         */
        [[nodiscard]]
        virtual long countMessages() const = 0;

        /**
         * @brief Retrieves the total count of messages of a topic.
         *
         * @param topicErn The Euclid resource name (ERN) of the topic whose messages are to be counted.
         * @return The total number of messages of the topic as a long integer.
         */
        [[nodiscard]]
        virtual long countMessages(const std::string &topicErn) const = 0;

        /**
         * @brief Removes all entries from the message repository, leaving it in an empty state.
         *
         * This method is intended to clear all stored data, and the repository will contain no entities after its execution.
         *
         * This is a pure virtual function and must be implemented by derived classes.
         */
        // virtual void clearMessages() = 0;

        /**
         * @brief Makes messages available again once their visibility timeout or delay has elapsed.
         *
         * A message that was handed out by receiveMessages() is in status "INVISIBLE" so other
         * receivers don't get it too. If the receiver never deletes it (e.g. it crashed before
         * acknowledging it), the message must become receivable again once its visibilityTimeout
         * has passed since it was claimed. Likewise, a message sent to a queue with delay > 0 is
         * in status "DELAYED" and must become receivable once its delayUntil has passed.
         * Called periodically by a background task.
         *
         * @return number of messages that were reset to status "AVAILABLE"
         */
        // virtual long resetExpiredMessages() = 0;
    };

}// namespace Euclid::Database