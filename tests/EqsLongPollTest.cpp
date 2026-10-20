// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EqsLongPollTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>
#include <thread>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>

using Euclid::Database::MongoEqsRepository;
using Euclid::Database::Entity::EQS::MessagePriority;
using Euclid::Database::Entity::EQS::Queue;
using namespace std::chrono_literals;

// A long-polling receive, as a consumer sees it: it comes back with a message as soon as one is
// sent, and empty at the end of its wait when none is. It used to get there by sleeping 100 ms
// between looks; it now waits to be woken, and these hold on to what it promised either way.
//
// On the in-memory store, so they need no database. That store has no change stream, so a send
// from this process is what wakes the receive here - the same Wake() the change stream calls for
// a send from another one.

namespace {

    using Clock = std::chrono::steady_clock;

    constexpr auto kQueue = "ern:eqs:eu-central-1:000000000000:production:queue:inbox";

    MongoEqsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEqsRepository{};
    }

    void createQueue(MongoEqsRepository &repo) {
        Queue queue;
        queue.name = "inbox";
        queue.ern = kQueue;
        queue.accountId = "000000000000";
        queue.nameSpace = "production";
        queue.region = "eu-central-1";
        queue.visibility = 30;
        queue.maxReceiveCount = 3;
        repo.upsertQueue(queue);
    }

}// namespace

BOOST_AUTO_TEST_CASE(AWaitingReceiveGetsAMessageSentDuringTheWait) {
    auto repo = freshRepository();
    createQueue(repo);

    std::thread producer([&repo] {
        std::this_thread::sleep_for(300ms);
        repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "{}", {}, {}, MessagePriority::MEDIUM);
    });

    const auto started = Clock::now();
    const auto received = repo.receiveMessages(kQueue, 10, 10);
    const auto took = Clock::now() - started;
    producer.join();

    BOOST_TEST_REQUIRE(received.size() == 1U);
    BOOST_TEST(received.front().messageId == "m-1");
    // Well before the ten-second wait would have run out.
    BOOST_TEST(took < 3s);
}

BOOST_AUTO_TEST_CASE(AReceiveOnAnEmptyQueueAnswersAtTheEndOfItsWait) {
    auto repo = freshRepository();
    createQueue(repo);

    const auto started = Clock::now();
    const auto received = repo.receiveMessages(kQueue, 10, 1);
    const auto took = Clock::now() - started;

    BOOST_TEST(received.empty());
    BOOST_TEST(took >= 1s);
    BOOST_TEST(took < 4s);
}

BOOST_AUTO_TEST_CASE(AReceiveWithoutAWaitAnswersAtOnce) {
    auto repo = freshRepository();
    createQueue(repo);

    const auto started = Clock::now();
    BOOST_TEST(repo.receiveMessages(kQueue, 10, 0).empty());
    BOOST_TEST((Clock::now() - started) < 1s);
}
