// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
#include <euclid/database/entity/eqs/Message.h>
#include <euclid/database/entity/eqs/MessagePriority.h>
#include <euclid/database/entity/eqs/Queue.h>

namespace Euclid::Database {

    /**
     * @brief Interface for SQS repository operations.
     *
     * Provides an abstraction for storing, retrieving, and managing
     * SQS-related data.
     */
    class IEqsRepository {

    public:

        /**
         * @brief Virtual destructor for the ISQSRepository interface.
         *
         * Ensures derived classes' destructor is invoked correctly
         * during object destruction to release resources.
         */
        virtual ~IEqsRepository() = default;

        /**
         * @brief Inserts a new queue or updates an existing one in the repository.
         *
         * If a module with the same identifier already exists, its data will be updated
         * with the provided queue information. Otherwise, a new queue will be added
         * to the repository.
         *
         * @param queue The queue to be inserted or updated in the repository.
         */
        virtual Entity::EQS::Queue upsertQueue(Entity::EQS::Queue &queue) = 0;

        /**
         * @brief Removes a queue, and its messages, by name within the account and namespace that
         * owns it.
         *
         * @param accountId account the queue belongs to.
         * @param nameSpace namespace within accountId; empty means the account's unscoped queues.
         * @param name The name of the queue to be removed.
         */
        virtual void removeQueueByName(const std::string &accountId, const std::string &nameSpace, const std::string &name) = 0;

        /**
         * @brief Removes the specified element or elements from the collection or data structure by ERN.
         *
         * @param ern The Euclid resource name (ERN) of the queue to be removed.
         */
        virtual void deleteQueueByErn(const std::string &ern) = 0;

        /**
         * @brief Searches for a queue by its name within the account and namespace that owns it.
         *
         * @par
         * A queue name is unique only within (accountId, nameSpace) - the same three fields the
         * unique index is built on - so all three are needed to name one queue.
         *
         * @param accountId account the queue belongs to.
         * @param nameSpace namespace within accountId the queue belongs to; empty means the
         * account's unscoped queues, not "any namespace".
         * @param name The name of the queue to search for.
         * @return The queue matching the given name, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EQS::Queue> findQueueByName(const std::string &accountId, const std::string &nameSpace, const std::string &name) const = 0;

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
        virtual std::optional<Entity::EQS::Queue> findQueueById(const std::string &oid) const = 0;

        /**
         * @brief Searches for a queue by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the queue to search for.
         * @return The item matching the given ERN, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EQS::Queue> findQueueByErn(const std::string &ern) const = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @param accountId only queues belonging to this account are returned
         * @param namespaceName only queues in this namespace are returned; empty means don't filter by namespace
         * @param prefix only queues whose name starts with this prefix are returned; empty matches all queues
         * @param pageSize maximum number of queues to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "arn"); empty means unsorted
         * @param sortDirection direction of sort by (e.g. "asc", "desc"); empty means unsorted
         * @param includeInternal whether euclid's own queues are listed as well; they are hidden by default (see Entity::EQS::Queue::internal)
         * @return A collection containing all entities or objects found.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EQS::Queue> listQueues(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection, bool includeInternal = false) const = 0;

        /**
         * @brief The queues that name this one as their dead letter queue.
         *
         * @par
         * This is what makes a queue a dead letter queue: nothing on the queue itself says so, the
         * relationship is only ever written the other way round. An empty answer therefore means
         * the queue is an ordinary one, which is what redrive-dlq refuses on.
         *
         * @param deadLetterQueueErn the queue to find the sources of.
         * @return the source queues, empty if this is not a dead letter queue.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EQS::Queue> listSourceQueues(const std::string &deadLetterQueueErn) const = 0;

        /**
         * @brief Moves messages out of a dead letter queue and back into a source queue.
         *
         * @par
         * The reverse of the move receiveMessages() makes when a message exceeds its receive
         * count, and undone the same way: the message's own queueErn is what moves it, and both
         * queues' counters come from the next scan. Its receive count goes back to zero, so a
         * redriven message gets the same number of attempts a new one would rather than returning
         * one failure away from being dead again.
         *
         * @param deadLetterQueueErn queue the messages are in now.
         * @param targetQueueErn queue to move them to.
         * @param sourceQueueErn only move messages recorded as having come from this queue; empty
         * moves every message in the dead letter queue, whatever it is recorded as.
         * @return how many messages were moved.
         */
        virtual long redriveMessages(const std::string &deadLetterQueueErn, const std::string &targetQueueErn,
                                     const std::string &sourceQueueErn) = 0;

        /**
         * @brief Checks if a queue with the specified name exists in the given account and
         * namespace.
         *
         * @param accountId account the queue would belong to.
         * @param nameSpace namespace within accountId; empty means the account's unscoped queues.
         * @param name The name of the queue to check for existence.
         * @return True if a queue with the given name exists there, otherwise false.
         */
        [[nodiscard]]
        virtual bool queueExists(const std::string &accountId, const std::string &nameSpace, const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of queues in the repository.
         *
         * @param accountId only queues belonging to this account are counted
         * @param namespaceName only queues in this namespace are counted; empty means don't filter by namespace
         * @param prefix only queues whose name starts with this prefix are counted; empty means don't filter by prefix
         * @param includeInternal whether euclid's own queues are counted as well; they are left out by default, so that the count matches the listing
         * @return The total number of queues as a long integer.
         */
        [[nodiscard]]
        virtual long countQueues(const std::string &accountId, const std::string &namespaceName, const std::string &prefix = "", bool includeInternal = false) const = 0;

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
        virtual void upsertMessage(const Entity::EQS::Message &message) = 0;

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
         * @param priority message priority; defaults to MEDIUM.
         * @return the newly created message entity.
         */
        virtual Entity::EQS::Message sendMessage(const std::string &messageId, const std::string &ern, const std::string &queueErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes, const std::map<std::string, Entity::COM::Variant> &systemAttributes, Entity::EQS::MessagePriority priority) = 0;

        /**
         * @brief Receives up to maxCount available messages from a queue.
         *
         * Available messages are claimed and moved to status "busy" (in-flight) before being
         * returned, so that concurrent receivers don't get the same message. If no messages are
         * immediately available and waitTime is greater than zero, the repository is polled
         * repeatedly (long polling) until either a message becomes available or waitTime seconds
         * have elapsed, whichever comes first.
         *
         * The maxCount slots are apportioned across the three priority tiers (HIGH/MEDIUM/LOW)
         * proportionally to the configurable weights returned by
         * Entity::EQS::LoadPriorityWeights() - see ComputeReceiveCounts() - so that with the
         * default weights, most of a batch is HIGH priority, fewer are MEDIUM, and fewer still are
         * LOW, while still filling up to maxCount whenever enough messages of any priority exist.
         *
         * @param queueErn ERN of the queue to receive messages from.
         * @param maxCount maximal number of messages to return.
         * @param waitTime maximal number of seconds to wait for messages to become available.
         * @return up to maxCount messages; empty if none became available within waitTime.
         */
        virtual std::vector<Entity::EQS::Message> receiveMessages(const std::string &queueErn, long maxCount, long waitTime) = 0;

        /**
         * @brief Deletes a message from the repository.
         *
         * @param receiptHandle The receipt handle of the message to delete.
         */
        virtual void deleteMessage(const std::string &receiptHandle) = 0;

        /**
         * @brief Deletes a message from the repository by its message ID, regardless of its
         * current status (AVAILABLE, DELAYED or INVISIBLE).
         *
         * Unlike deleteMessage(), this does not require the message to have been received first,
         * i.e. it bypasses the usual receipt-handle lease/lock semantics. This is a Euclid-specific
         * extension; AWS SQS has no equivalent operation.
         *
         * @param messageId The message ID of the message to delete.
         */
        virtual void deleteMessageById(const std::string &messageId) = 0;

        /**
         * @brief Deletes all messages of a queue.
         *
         * @param queueErn The Euclid resource name (ERN) of the queue whose messages are to be purged.
         */
        virtual void purgeQueue(const std::string &queueErn) = 0;

        /**
         * @brief Deletes all messages of every queue in a region/account/nameSpace.
         *
         * @param region region of the queues to purge.
         * @param accountId account ID of the queues to purge.
         * @param nameSpace namespace of the queues to purge; empty purges every namespace of the
         * account.
         */
        virtual void purgeAllQueues(const std::string &region, const std::string &accountId, const std::string &nameSpace) = 0;

        /**
         * @brief Searches for a message by its name.
         *
         * @param name The name of the message to search for.
         * @return The item matching the given name, or nullptr if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EQS::Message> findMessageByName(const std::string &name) const = 0;

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
        virtual std::optional<Entity::EQS::Message> findMessageById(const std::string &oid) const = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @return A collection containing all entities or objects found.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EQS::Message> findAllMessages() const = 0;

        /**
         * @brief Lists the messages of a queue, without receiving them (i.e. without changing
         * their status or visibility), paginated and sorted.
         *
         * @param queueErn ERN of the queue whose messages are listed.
         * @param pageSize maximum number of messages to return, or <= 0 for no limit.
         * @param pageIndex zero-based page index, combined with pageSize to compute the offset.
         * @param sortColumn message field to sort ascending by, e.g. "created", "size",
         * @param sortDirection direction of sort by (e.g. "asc", "desc"); empty means unsorted
         * "messageId"; unrecognized/empty leaves the order unspecified.
         * @return the requested page of messages.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EQS::Message> listMessages(const std::string &queueErn, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const = 0;

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

        /**
         * @brief Adds to a queue's counters, leaving every other field alone.
         *
         * @par
         * The counters a queue reports - available, delayed, invisible and the byte total - are
         * maintained here, by the operations that change them, so they are right as each one
         * happens rather than as of the last scan. One `$inc`, so two adjustments that overlap
         * both land instead of one overwriting the other, and nothing else on the document is
         * touched - a stale copy cannot revert a setting somebody changed meanwhile.
         *
         * @par
         * This is the whole seam. Every send, receive, delete, reset and redrive goes through it,
         * so what a queue reports can only drift by what happens behind the application's back -
         * which is the TTL index removing expired messages, and a process dying between writing a
         * message and adjusting for it. @ref recountQueues corrects both.
         *
         * @par
         * Counted per call rather than per message where a call handles several: a receive that
         * claims ten messages adjusts once. Writes to one queue's row are also coalesced over a
         * short interval by the implementation - that row is the one thing every producer and
         * consumer of a queue shares, and writing it once per message is what made this
         * unaffordable the first time round.
         *
         * @param queueErn queue to adjust
         * @param availableDelta change to the number of receivable messages
         * @param invisibleDelta change to the number of claimed messages
         * @param delayedDelta change to the number of messages not yet receivable
         * @param sizeDelta change to the stored byte total
         */
        virtual void adjustQueueCounters(const std::string &queueErn, long availableDelta, long invisibleDelta,
                                         long delayedDelta, long sizeDelta) = 0;

        /**
         * @brief Writes out any counter adjustments this process is still holding.
         *
         * @par
         * Adjustments are coalesced, so the last few of them are in memory rather than in the
         * database at any moment. Called before a process stops, and by anything that has to read
         * its own writes - a test, or a handler answering with the counters it just changed.
         */
        virtual void flushQueueCounters() = 0;

        /**
         * @brief Recounts every queue's messages and stores the result on the queues.
         *
         * @par
         * The safety net under @ref adjustQueueCounters, not the source of the numbers. Two things
         * change a queue's contents without an adjustment to go with them: the TTL index removing
         * messages the moment they expire, which no application code sees; and a process dying
         * between writing a message and accounting for it. Both leave the counters wrong in a way
         * only a count from truth can put right, which is what this is.
         *
         * @par
         * It reads every message in the installation, so it is not free and gets less free as the
         * queues fill - `euclid.modules.emo.queue-count-period` decides how often that is paid.
         * Since the counters are maintained as they change, this can run rarely: it is correcting
         * expiry and crashes, not counting the traffic.
         *
         * @par
         * Called by EMO, which runs as a single instance and already owns the installation's
         * periodic measurement. Deliberately not called by the queueing module itself: that runs
         * many instances, and each would repeat the same scan.
         */
        virtual void recountQueues() = 0;
    };

}// namespace Euclid::Database