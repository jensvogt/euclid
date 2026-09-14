// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EnsResendTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <map>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/entity/ens/Message.h>
#include <euclid/database/entity/ens/Topic.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>

using Euclid::Database::MongoEnsRepository;
using Euclid::Database::Entity::ENS::Message;
using Euclid::Database::Entity::ENS::Topic;
using Euclid::Database::Entity::ENS::kStatusHeld;

// A topic keeps what was published to it for its retention period, and once a subscriber has
// consumed the queue message that record is the only copy left. resend-messages is the way back to
// it - for a subscriber that was down, that subscribed after the fact, or that acknowledged
// something it then failed to process.
//
// What this pins is the state the handler reads to decide what to hand over, because getting it
// wrong is invisible: resending a held message delivers something that was never delivered and
// leaves it marked held, so the next start-topic delivers it a second time.

namespace {

    constexpr auto kTopicErn = "ern:ens:eu-central-1:000000000000:production:topic:orders";
    constexpr auto kAccount = "000000000000";

    MongoEnsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEnsRepository{};
    }

    Topic topicOf(MongoEnsRepository &repo, const bool delivering = true) {
        Topic topic;
        topic.name = "orders";
        topic.ern = kTopicErn;
        topic.accountId = kAccount;
        topic.nameSpace = "production";
        topic.region = "eu-central-1";
        topic.delivering = delivering;
        return repo.upsertTopic(topic);
    }

    Message publish(MongoEnsRepository &repo, const std::string &messageId, const std::string &body) {
        const std::map<std::string, Euclid::Database::Entity::COM::Variant> attributes;
        return repo.publishMessage(messageId, "ern:ens:eu-central-1:000000000000:message:" + messageId,
                                   kTopicErn, body, attributes, "MEDIUM");
    }

    std::vector<std::string> bodiesInOrder(const MongoEnsRepository &repo) {
        std::vector<std::string> bodies;
        for (const auto &message: repo.listMessages(kTopicErn, 500, 0, "created", "asc")) {
            bodies.push_back(message.body);
        }
        return bodies;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ADeliveringTopicKeepsItsMessagesAfterTheyAreDelivered) {

    // The premise the whole command rests on: a fan-out does not consume the topic's copy. Without
    // this there would be nothing to resend.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    std::ignore = publish(repo, "m-1", "first");
    std::ignore = publish(repo, "m-2", "second");

    BOOST_TEST(repo.listMessages(kTopicErn, 500, 0, "created", "asc").size() == 2U);
}

BOOST_AUTO_TEST_CASE(MessagesComeBackInPublishOrder) {

    // Resent oldest first, because a subscriber receiving a backlog out of order is worse than not
    // receiving it - this is the sort the handler asks for.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    std::ignore = publish(repo, "m-1", "first");
    std::ignore = publish(repo, "m-2", "second");
    std::ignore = publish(repo, "m-3", "third");

    const auto bodies = bodiesInOrder(repo);
    BOOST_REQUIRE(bodies.size() == 3U);
    BOOST_TEST(bodies[0] == "first");
    BOOST_TEST(bodies[1] == "second");
    BOOST_TEST(bodies[2] == "third");
}

BOOST_AUTO_TEST_CASE(AMessagePublishedToAStoppedTopicIsHeld) {

    // The state resend-messages must skip. A held message has never been delivered at all, so
    // handing it over from here would deliver it without marking it - and start-topic, which is
    // what marks it, would then deliver it a second time.
    auto repo = freshRepository();
    std::ignore = topicOf(repo, false);

    const auto message = publish(repo, "m-1", "while stopped");

    BOOST_TEST(message.status == kStatusHeld);
}

BOOST_AUTO_TEST_CASE(AMessagePublishedToADeliveringTopicIsNotHeld) {

    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    const auto message = publish(repo, "m-1", "while delivering");

    BOOST_TEST(message.status != kStatusHeld);
}

BOOST_AUTO_TEST_CASE(AMessageKnowsWhichTopicItBelongsTo) {

    // What the by-id form checks before resending. Without it, naming any message id would fan that
    // message out to the subscriptions of a topic it was never published to.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);
    std::ignore = publish(repo, "m-1", "first");

    const auto found = repo.findMessageById("m-1");

    BOOST_REQUIRE(found.has_value());
    BOOST_TEST(found->topicErn == kTopicErn);
    BOOST_TEST(!repo.findMessageById("no-such-message").has_value());
}

BOOST_AUTO_TEST_CASE(TheResendCounterIsALifetimeTotal) {

    // Shared with start-topic, which records the same way - so "resend" on a topic counts every
    // message handed over a second time, however it was asked for.
    auto repo = freshRepository();
    auto topic = topicOf(repo);
    BOOST_TEST(topic.resend == 0L);

    repo.recordResend(kTopicErn, 3);
    repo.recordResend(kTopicErn, 2);

    const auto found = repo.findTopicByErn(kTopicErn);
    BOOST_REQUIRE(found.has_value());
    BOOST_TEST(found->resend == 5L);
}

BOOST_AUTO_TEST_CASE(PagingWalksForwardBecauseNothingIsRemoved) {

    // Unlike a purge, which always asks for page zero because it deletes as it goes, a resend
    // leaves everything where it is - so the page after the one just read really is the next one.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    for (int i = 0; i < 12; ++i) std::ignore = publish(repo, "m-" + std::to_string(i), "body-" + std::to_string(i));

    std::vector<std::string> seen;
    for (long pageIndex = 0;; ++pageIndex) {
        const auto page = repo.listMessages(kTopicErn, 5, pageIndex, "created", "asc");
        if (page.empty()) break;
        for (const auto &message: page) seen.push_back(message.messageId);
        if (page.size() < 5U) break;
    }

    BOOST_TEST(seen.size() == 12U);
    const bool anyDuplicates = std::ranges::adjacent_find(seen) != seen.end();
    BOOST_TEST(!anyDuplicates);
}
