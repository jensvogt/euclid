// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE VisibilityTimeoutTest
#include <boost/test/unit_test.hpp>

#include <string>

#include <euclid/database/Database.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>

using Euclid::Database::MongoEqsRepository;
using Euclid::Database::Entity::EQS::MessagePriority;
using Euclid::Database::Entity::EQS::Queue;

// Which visibility timeout a receive leases a message for: the queue's, as it is at the receive.
//
// It used to be the one copied onto the message when it was sent. A topic queue created with the
// default of 30 seconds and set to 90 an hour later went on leasing 30 for every message already in
// it, to a consumer needing about 40 - so every message expired mid-export, was received again by
// another poller, and went to the dead-letter queue on its third receive.

namespace {

    constexpr auto kQueue = "ern:eqs:eu-central-1:000000000000:production:queue:exports";

    MongoEqsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEqsRepository{};
    }

    Queue queueWith(MongoEqsRepository &repo, const long visibility) {
        Queue queue;
        queue.name = "exports";
        queue.ern = kQueue;
        queue.accountId = "000000000000";
        queue.nameSpace = "production";
        queue.region = "eu-central-1";
        queue.visibility = visibility;
        queue.maxReceiveCount = 3;
        return repo.upsertQueue(queue);
    }

    long storedVisibility(const MongoEqsRepository &repo, const std::string &messageId) {
        const auto stored = repo.findMessageByName(messageId);
        BOOST_TEST_REQUIRE(stored.has_value());
        return stored->visibilityTimeout;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ARaisedQueueVisibilityAppliesToMessagesAlreadyInTheQueue) {
    auto repo = freshRepository();
    queueWith(repo, 30);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "{}", {}, {}, MessagePriority::MEDIUM);

    // Raised after the send, as an operator would on finding the consumer too slow for it.
    queueWith(repo, 90);

    const auto received = repo.receiveMessages(kQueue, 1, 0);
    BOOST_TEST_REQUIRE(received.size() == 1u);

    // Both what the consumer is told and what the expiry sweep reads.
    BOOST_TEST(received.front().visibilityTimeout == 90);
    BOOST_TEST(storedVisibility(repo, "m-1") == 90);
}

BOOST_AUTO_TEST_CASE(ALoweredQueueVisibilityAppliesToo) {
    auto repo = freshRepository();
    queueWith(repo, 300);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "{}", {}, {}, MessagePriority::MEDIUM);
    queueWith(repo, 60);

    const auto received = repo.receiveMessages(kQueue, 1, 0);
    BOOST_TEST_REQUIRE(received.size() == 1u);
    BOOST_TEST(received.front().visibilityTimeout == 60);
}

BOOST_AUTO_TEST_CASE(AnUnchangedQueueLeasesWhatItAlwaysDid) {
    auto repo = freshRepository();
    queueWith(repo, 45);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "{}", {}, {}, MessagePriority::MEDIUM);

    const auto received = repo.receiveMessages(kQueue, 1, 0);
    BOOST_TEST_REQUIRE(received.size() == 1u);
    BOOST_TEST(received.front().visibilityTimeout == 45);
    BOOST_TEST(storedVisibility(repo, "m-1") == 45);
}

BOOST_AUTO_TEST_CASE(ATimeoutSetOnTheMessageBeforeItIsReceivedIsKept) {

    // set-message-visibility on a message not yet in flight - EQS's extension of AWS's call. That
    // is somebody asking for this message to be leased for longer, and the queue's figure applied
    // at the receive must not undo it.
    auto repo = freshRepository();
    queueWith(repo, 30);
    auto message = repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "{}", {}, {}, MessagePriority::MEDIUM);
    message = *repo.findMessageByName("m-1");
    message.visibilityTimeout = 300;
    message.visibilityPinned = true;
    repo.upsertMessage(message);

    const auto received = repo.receiveMessages(kQueue, 1, 0);
    BOOST_TEST_REQUIRE(received.size() == 1u);
    BOOST_TEST(received.front().visibilityTimeout == 300);
    BOOST_TEST(storedVisibility(repo, "m-1") == 300);
}
