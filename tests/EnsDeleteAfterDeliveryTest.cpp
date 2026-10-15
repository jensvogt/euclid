// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EnsDeleteAfterDeliveryTest
#include <boost/test/unit_test.hpp>

#include <string>

#include <euclid/database/Database.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>

using Euclid::Database::MongoEnsRepository;
using Euclid::Database::Entity::ENS::Topic;

// A topic can be set to delete each message once it has been handed to every subscription, rather
// than keeping it for the retention period. These pin the repository's half: the setting is stored
// and read back, a topic that has never been told keeps behaving as topics always have, and
// deleting a delivered message takes it off the topic's counters exactly once.

namespace {

    constexpr auto kTopic = "ern:ens:eu-central-1:000000000000:production:topic:orders";

    MongoEnsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEnsRepository{};
    }

    Topic topic(MongoEnsRepository &repo, const bool deleteAfterDelivery = false) {
        Topic topic;
        topic.name = "orders";
        topic.ern = kTopic;
        topic.accountId = "000000000000";
        topic.nameSpace = "production";
        topic.region = "eu-central-1";
        topic.deleteAfterDelivery = deleteAfterDelivery;
        return repo.upsertTopic(topic);
    }

    Topic reread(const MongoEnsRepository &repo) {
        const auto stored = repo.findTopicByErn(kTopic);
        BOOST_TEST_REQUIRE(stored.has_value());
        return *stored;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ATopicKeepsItsMessagesUnlessToldOtherwise) {

    // The current behaviour, and what a topic written before the setting existed reads back as.
    auto repo = freshRepository();
    topic(repo);

    BOOST_TEST(!reread(repo).deleteAfterDelivery);
}

BOOST_AUTO_TEST_CASE(TheSettingIsStoredAndCanBeTurnedOffAgain) {
    auto repo = freshRepository();
    topic(repo, true);
    BOOST_TEST(reread(repo).deleteAfterDelivery);

    auto stored = reread(repo);
    stored.deleteAfterDelivery = false;
    repo.upsertTopic(stored);
    BOOST_TEST(!reread(repo).deleteAfterDelivery);
}

BOOST_AUTO_TEST_CASE(ADeliveredMessageIsDeletedAndTakenOffTheTopicsCounters) {
    auto repo = freshRepository();
    topic(repo, true);
    repo.publishMessage("m-1", "ern:ens:...:message:m-1", kTopic, "0123456789", {}, "MEDIUM");
    BOOST_TEST_REQUIRE(reread(repo).available == 1);
    BOOST_TEST_REQUIRE(reread(repo).size == 10);

    BOOST_TEST(repo.deleteDeliveredMessage(kTopic, "m-1", 10));

    BOOST_TEST(!repo.findMessageById("m-1").has_value());
    BOOST_TEST(reread(repo).available == 0);
    BOOST_TEST(reread(repo).size == 0);
}

BOOST_AUTO_TEST_CASE(AMessageAlreadyGoneIsNotSubtractedTwice) {

    // Expired, purged, or deleted by another ENS instance first: the delete removes nothing, and
    // the counters must not move for it - a topic would otherwise drift below what it holds.
    auto repo = freshRepository();
    topic(repo, true);
    repo.publishMessage("m-1", "ern:ens:...:message:m-1", kTopic, "0123456789", {}, "MEDIUM");
    repo.publishMessage("m-2", "ern:ens:...:message:m-2", kTopic, "abcde", {}, "MEDIUM");
    BOOST_TEST_REQUIRE(repo.deleteDeliveredMessage(kTopic, "m-1", 10));

    BOOST_TEST(!repo.deleteDeliveredMessage(kTopic, "m-1", 10));

    BOOST_TEST(reread(repo).available == 1);
    BOOST_TEST(reread(repo).size == 5);
    BOOST_TEST(repo.findMessageById("m-2").has_value());
}

// ── Purging starts a topic's counters over ──────────────────────────────────

BOOST_AUTO_TEST_CASE(APurgeResetsTheSendAndResendTotalsToo) {

    // What the topic holds and what it has sent, both from zero: a topic purged empty used to go on
    // reading as having sent thousands of messages it no longer had any trace of.
    auto repo = freshRepository();
    topic(repo);
    repo.publishMessage("m-1", "ern:ens:...:message:m-1", kTopic, "0123456789", {}, "MEDIUM");
    repo.recordSend(kTopic, 3);
    repo.recordResend(kTopic, 2);
    BOOST_TEST_REQUIRE(reread(repo).send == 3);
    BOOST_TEST_REQUIRE(reread(repo).resend == 2);

    repo.purgeTopic(kTopic);

    const auto purged = reread(repo);
    BOOST_TEST(purged.available == 0);
    BOOST_TEST(purged.size == 0);
    BOOST_TEST(purged.send == 0);
    BOOST_TEST(purged.resend == 0);
}

BOOST_AUTO_TEST_CASE(APurgeOfAllTopicsResetsTheirTotalsToo) {
    auto repo = freshRepository();
    topic(repo);
    repo.recordSend(kTopic, 5);
    repo.recordResend(kTopic, 1);

    repo.purgeAllTopics("eu-central-1", "000000000000", "production");

    BOOST_TEST(reread(repo).send == 0);
    BOOST_TEST(reread(repo).resend == 0);
}
