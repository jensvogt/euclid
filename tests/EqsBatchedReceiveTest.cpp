// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EqsBatchedReceiveTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <set>
#include <string>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>

using Euclid::Database::MongoEqsRepository;
using Euclid::Database::Entity::EQS::Message;
using Euclid::Database::Entity::EQS::MessagePriority;
using Euclid::Database::Entity::EQS::MessageStatus;
using Euclid::Database::Entity::EQS::Queue;

// A receive claims a batch per priority now - find, one bulk write, read back - where it used to
// claim one message at a time. What a consumer can tell apart is unchanged, and that is what these
// hold on to: every message handed out is its own, with a receipt handle of its own that deletes it
// and nothing else, and a message past its receive allowance still goes to the dead letter queue
// instead of to the consumer.
//
// On the in-memory store, so they need no database.

namespace {

    constexpr auto kQueue = "ern:eqs:eu-central-1:000000000000:production:queue:orders";
    constexpr auto kDlq = "ern:eqs:eu-central-1:000000000000:production:queue:orders-dlq";

    MongoEqsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEqsRepository{};
    }

    void addQueue(MongoEqsRepository &repo, const std::string &ern, const std::string &dlqErn = {}) {
        Queue queue;
        queue.ern = ern;
        queue.name = ern.substr(ern.rfind(':') + 1);
        queue.accountId = "000000000000";
        queue.nameSpace = "production";
        queue.region = "eu-central-1";
        queue.visibility = 30;
        queue.maxReceiveCount = 3;
        queue.deadLetterQueueErn = dlqErn;
        repo.upsertQueue(queue);
    }

    void send(MongoEqsRepository &repo, const std::string &messageId, const MessagePriority priority = MessagePriority::MEDIUM) {
        repo.sendMessage(messageId, "ern:eqs:...:message:" + messageId, kQueue, "{}", {}, {}, priority);
    }

    // A message that has already been received as often as the queue allows: the next receive
    // takes it past the allowance.
    void addExhausted(MongoEqsRepository &repo, const std::string &messageId) {
        Message message;
        message.messageId = messageId;
        message.ern = "ern:eqs:...:message:" + messageId;
        message.queueErn = kQueue;
        message.body = "{}";
        message.status = MessageStatus::AVAILABLE;
        message.priority = MessagePriority::MEDIUM;
        message.receivedCount = 3;
        repo.upsertMessage(message);
    }

}// namespace

BOOST_AUTO_TEST_CASE(EveryMessageInABatchGetsItsOwnReceiptHandle) {
    auto repo = freshRepository();
    addQueue(repo, kQueue);
    for (int n = 0; n < 25; ++n) send(repo, "m-" + std::to_string(n));

    const auto received = repo.receiveMessages(kQueue, 10, 0);
    BOOST_TEST_REQUIRE(received.size() == 10U);

    std::set<std::string> handles;
    std::set<std::string> ids;
    for (const auto &message: received) {
        BOOST_TEST(!message.receiptHandle.empty());
        BOOST_TEST(message.receivedCount == 1);
        handles.insert(message.receiptHandle);
        ids.insert(message.messageId);

        const auto stored = repo.findMessageByName(message.messageId);
        BOOST_TEST_REQUIRE(stored.has_value());
        BOOST_TEST((stored->status == MessageStatus::INVISIBLE));
        BOOST_TEST(stored->receiptHandle == message.receiptHandle);
    }
    // A handle shared between two messages would make deleting one delete whichever the database
    // found first.
    BOOST_TEST(handles.size() == 10U);
    BOOST_TEST(ids.size() == 10U);
}

BOOST_AUTO_TEST_CASE(AReceiptHandleDeletesExactlyItsOwnMessage) {
    auto repo = freshRepository();
    addQueue(repo, kQueue);
    for (int n = 0; n < 5; ++n) send(repo, "m-" + std::to_string(n));

    const auto received = repo.receiveMessages(kQueue, 5, 0);
    BOOST_TEST_REQUIRE(received.size() == 5U);

    repo.deleteMessage(received[2].receiptHandle);

    BOOST_TEST(!repo.findMessageByName(received[2].messageId).has_value());
    for (const std::size_t index: {0U, 1U, 3U, 4U}) {
        BOOST_TEST(repo.findMessageByName(received[index].messageId).has_value());
    }
}

BOOST_AUTO_TEST_CASE(TwoReceivesNeverShareAMessage) {
    auto repo = freshRepository();
    addQueue(repo, kQueue);
    for (int n = 0; n < 6; ++n) send(repo, "m-" + std::to_string(n));

    const auto first = repo.receiveMessages(kQueue, 4, 0);
    const auto second = repo.receiveMessages(kQueue, 4, 0);
    const auto third = repo.receiveMessages(kQueue, 4, 0);

    BOOST_TEST(first.size() == 4U);
    BOOST_TEST(second.size() == 2U);
    BOOST_TEST(third.empty());

    std::set<std::string> ids;
    for (const auto &message: first) ids.insert(message.messageId);
    for (const auto &message: second) ids.insert(message.messageId);
    BOOST_TEST(ids.size() == 6U);
}

BOOST_AUTO_TEST_CASE(EveryPriorityIsServed) {
    auto repo = freshRepository();
    addQueue(repo, kQueue);
    send(repo, "high", MessagePriority::HIGH);
    send(repo, "medium", MessagePriority::MEDIUM);
    send(repo, "low", MessagePriority::LOW);

    // Each tier claimed as a batch of its own; a receive with room for all three gets all three.
    const auto received = repo.receiveMessages(kQueue, 10, 0);

    std::set<std::string> ids;
    for (const auto &message: received) ids.insert(message.messageId);
    BOOST_TEST((ids == std::set<std::string>{"high", "medium", "low"}));
}

BOOST_AUTO_TEST_CASE(AnExhaustedMessageGoesToTheDeadLetterQueueAndTheRestToTheCaller) {
    auto repo = freshRepository();
    addQueue(repo, kDlq);
    addQueue(repo, kQueue, kDlq);
    addExhausted(repo, "dead-1");
    addExhausted(repo, "dead-2");
    send(repo, "live-1");
    send(repo, "live-2");
    send(repo, "live-3");

    const auto received = repo.receiveMessages(kQueue, 10, 0);

    std::set<std::string> ids;
    for (const auto &message: received) ids.insert(message.messageId);
    BOOST_TEST((ids == std::set<std::string>{"live-1", "live-2", "live-3"}));

    // Moved in one write for the batch, and moved properly: available in the dead letter queue,
    // with no lease, a fresh allowance, and a record of where it came from for a redrive.
    for (const auto *id: {"dead-1", "dead-2"}) {
        const auto stored = repo.findMessageByName(id);
        BOOST_TEST_REQUIRE(stored.has_value());
        BOOST_TEST(stored->queueErn == kDlq);
        BOOST_TEST(stored->sourceQueueErn == kQueue);
        BOOST_TEST((stored->status == MessageStatus::AVAILABLE));
        BOOST_TEST(stored->receivedCount == 0);
        BOOST_TEST(stored->receiptHandle.empty());
    }
}

BOOST_AUTO_TEST_CASE(ADeadLetteredMessageDoesNotUseUpTheCallersShare) {
    auto repo = freshRepository();
    addQueue(repo, kDlq);
    addQueue(repo, kQueue, kDlq);
    addExhausted(repo, "dead-1");
    addExhausted(repo, "dead-2");
    for (int n = 0; n < 5; ++n) send(repo, "live-" + std::to_string(n));

    // Room for three. If the first pass happens to find the two exhausted messages, they go to the
    // dead letter queue and the next pass fills the gap - the caller still gets three.
    const auto received = repo.receiveMessages(kQueue, 3, 0);
    BOOST_TEST(received.size() == 3U);
    for (const auto &message: received) BOOST_TEST(message.messageId.rfind("live-", 0) == 0U);
}
