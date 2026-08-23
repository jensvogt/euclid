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
#include <euclid/database/entity/queues/Message.h>
#include <euclid/database/entity/queues/MessagePriority.h>
#include <euclid/database/entity/queues/Queue.h>
#include <euclid/database/entity/queues/Variant.h>

namespace Euclid::Database {

    /**
     * @brief Interface for SQS repository operations.
     *
     * Provides an abstraction for storing, retrieving, and managing
     * SQS-related data.
     */
    class IQueuesRepository {

    public:

        /**
         * @brief Virtual destructor for the ISQSRepository interface.
         *
         * Ensures derived classes' destructor is invoked correctly
         * during object destruction to release resources.
         */
        virtual ~IQueuesRepository() = default;

        /**
         * @brief Inserts a new queue or updates an existing one in the repository.
         *
         * If a module with the same identifier already exists, its data will be updated
         * with the provided queue information. Otherwise, a new queue will be added
         * to the repository.
         *
         * @param queue The queue to be inserted or updated in the repository.
         */
        virtual Entity::Queues::Queue upsertQueue(Entity::Queues::Queue &queue) = 0;

        /**
         * @brief Removes the specified element or elements from the collection or data structure.
         *
         * @param name The name of the queue to be removed.
         */
        virtual void removeQueueByName(const std::string &name) = 0;

        /**
         * @brief Removes the specified element or elements from the collection or data structure by ERN.
         *
         * @param ern The Euclid resource name (ERN) of the queue to be removed.
         */
        virtual void deleteQueueByErn(const std::string &ern) = 0;

        /**
         * @brief Searches for a queue by its name.
         *
         * @param name The name of the queue to search for.
         * @return The item matching the given name, or nullptr if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::Queues::Queue> findQueueByName(const std::string &name) const = 0;

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
        [[nodiscard]]
        virtual std::optional<Entity::Queues::Queue> findQueueById(const std::string &oid) const = 0;

        /**
         * @brief Searches for a queue by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the queue to search for.
         * @return The item matching the given ERN, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::Queues::Queue> findQueueByErn(const std::string &ern) const = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @param prefix only queues whose name starts with this prefix are returned; empty matches all queues
         * @param pageSize maximum number of queues to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "arn"); empty means unsorted
         * @return A collection containing all entities or objects found.
         */
        [[nodiscard]]
        virtual std::vector<Entity::Queues::Queue> listQueues(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Checks if a queue with the specified name exists in the repository.
         *
         * @param name The name of the queue to check for existence.
         * @return True if a queue with the given name exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool queueExists(const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of queues in the repository.
         *
         * @return The total number of queues as a long integer.
         */
        [[nodiscard]]
        virtual long countQueues() const = 0;

        /**
         * @brief Removes all entries from the queue repository, leaving it in an empty state.
         *
         * This method is intended to clear all stored data, and the repository will contain no entities after its execution.
         *
         * This is a pure virtual function and must be implemented by derived classes.
         */
        virtual void clearQueues() = 0;

        /**
         * @brief Inserts a new message or updates an existing one in the repository.
         *
         * If a module with the same identifier already exists, its data will be updated
         * with the provided message information. Otherwise, a new message will be added
         * to the repository.
         *
         * @param message The message to be inserted or updated in the repository.
         */
        virtual void upsertMessage(const Entity::Queues::Message &message) = 0;

        /**
         * @brief Sends a message to a queue.
         *
         * Builds a new message entity for the queue identified by its ERN, assigns it
         * a message ID, persists it in the repository, and returns the persisted entity.
         *
         * @param messageId message ID
         * @param ern ERN of the message
         * @param queueErn ERN of the queue the message is sent to.
         * @param body message body.
         * @param attributes message attributes.
         * @param priority message priority; defaults to MIDDLE.
         * @return the newly created message entity.
         */
        virtual Entity::Queues::Message sendMessage(const std::string &messageId, const std::string &ern, const std::string &queueErn, const std::string &body, const std::map<std::string, Entity::Queues::Variant> &attributes, Entity::Queues::MessagePriority priority = Entity::Queues::MessagePriority::MIDDLE) = 0;

        /**
         * @brief Receives up to maxCount available messages from a queue.
         *
         * Available messages are claimed and moved to status "busy" (in-flight) before being
         * returned, so that concurrent receivers don't get the same message. If no messages are
         * immediately available and waitTime is greater than zero, the repository is polled
         * repeatedly (long polling) until either a message becomes available or waitTime seconds
         * have elapsed, whichever comes first.
         *
         * The maxCount slots are apportioned across the three priority tiers (HIGH/MIDDLE/LOW)
         * proportionally to the configurable weights returned by
         * Entity::Queues::LoadPriorityWeights() - see ComputeReceiveCounts() - so that with the
         * default weights, most of a batch is HIGH priority, fewer are MIDDLE, and fewer still are
         * LOW, while still filling up to maxCount whenever enough messages of any priority exist.
         *
         * @param queueErn ERN of the queue to receive messages from.
         * @param maxCount maximal number of messages to return.
         * @param waitTime maximal number of seconds to wait for messages to become available.
         * @return up to maxCount messages; empty if none became available within waitTime.
         */
        virtual std::vector<Entity::Queues::Message> receiveMessages(const std::string &queueErn, long maxCount, long waitTime) = 0;

        /**
         * @brief Deletes a message from the repository.
         *
         * @param receiptHandle The receipt handle of the message to delete.
         */
        virtual void deleteMessage(const std::string &receiptHandle) = 0;

        /**
         * @brief Deletes all messages of a queue.
         *
         * @param queueErn The Euclid resource name (ERN) of the queue whose messages are to be purged.
         */
        virtual void purgeQueue(const std::string &queueErn) = 0;

        /**
         * @brief Deletes all messages of every queue in a region/account.
         *
         * @param region region of the queues to purge.
         * @param accountId account ID of the queues to purge.
         */
        virtual void purgeAllQueues(const std::string &region, const std::string &accountId) = 0;

        /**
         * @brief Searches for a message by its name.
         *
         * @param name The name of the message to search for.
         * @return The item matching the given name, or nullptr if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::Queues::Message> findMessageByName(const std::string &name) const = 0;

        /**
         * @brief Locates a message in the repository by its unique identifier.
         *
         * Searches for a message matching the specified identifier and returns it
         * if found. If no matching module exists, an empty optional is returned.
         *
         * @param oid The unique identifier of the message to search for.
         * @return An optional containing the found message, or an empty optional
         *         if no module with the given identifier exists.
         */
        [[nodiscard]]
        virtual std::optional<Entity::Queues::Message> findMessageById(const std::string &oid) const = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @return A collection containing all entities or objects found.
         */
        [[nodiscard]]
        virtual std::vector<Entity::Queues::Message> findAllMessages() const = 0;

        /**
         * @brief Lists the messages of a queue, without receiving them (i.e. without changing
         * their status or visibility), paginated and sorted.
         *
         * @param queueErn ERN of the queue whose messages are listed.
         * @param pageSize maximum number of messages to return, or <= 0 for no limit.
         * @param pageIndex zero-based page index, combined with pageSize to compute the offset.
         * @param sortColumn message field to sort ascending by, e.g. "created", "size",
         * "messageId"; unrecognized/empty leaves the order unspecified.
         * @return the requested page of messages.
         */
        [[nodiscard]]
        virtual std::vector<Entity::Queues::Message> listMessages(const std::string &queueErn, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Checks if a message with the specified name exists in the repository.
         *
         * @param name The name of the message to check for existence.
         * @return True if a message with the given name exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool messageExists(const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of messages in the repository.
         *
         * @return The total number of messages as a long integer.
         */
        [[nodiscard]]
        virtual long countMessages() const = 0;

        /**
         * @brief Retrieves the total count of messages of a queue.
         *
         * @param queueErn The Euclid resource name (ERN) of the queue whose messages are to be counted.
         * @return The total number of messages of the queue as a long integer.
         */
        [[nodiscard]]
        virtual long countMessages(const std::string &queueErn) const = 0;

        /**
         * @brief Removes all entries from the message repository, leaving it in an empty state.
         *
         * This method is intended to clear all stored data, and the repository will contain no entities after its execution.
         *
         * This is a pure virtual function and must be implemented by derived classes.
         */
        virtual void clearMessages() = 0;

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
        virtual long resetExpiredMessages() = 0;
    };

}// namespace Euclid::Database