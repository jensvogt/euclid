// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EnsResendTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <chrono>
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

    // Exactly the walk resendAllMessages() performs: pages of pageSize, each starting after the
    // last message of the one before.
    std::vector<std::string> walkInPages(const MongoEnsRepository &repo, const long pageSize) {

        std::vector<std::string> bodies;
        std::string afterOid;

        for (;;) {
            const auto page = repo.listMessagesAfter(kTopicErn, pageSize, afterOid);
            if (page.empty()) break;

            for (const auto &message: page) bodies.push_back(message.body);

            afterOid = page.back().oid;
            if (static_cast<long>(page.size()) < pageSize) break;
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

BOOST_AUTO_TEST_CASE(TheSendCounterIsALifetimeTotalToo) {

    // The half that was missing. "send" existed on the entity, in the DTO, in the mapper and as a
    // column in the RUI, and nothing anywhere incremented it - so every topic in the installation
    // read send=0 while the resend beside it climbed. Observed on protokollierung-topic:
    // send 0, resend 2,157,000, over 1.4 million messages published.
    auto repo = freshRepository();
    auto topic = topicOf(repo);
    BOOST_TEST(topic.send == 0L);

    repo.recordSend(kTopicErn, 1);
    repo.recordSend(kTopicErn, 1);
    repo.recordSend(kTopicErn, 1);

    const auto found = repo.findTopicByErn(kTopicErn);
    BOOST_REQUIRE(found.has_value());
    BOOST_TEST(found->send == 3L);
}

BOOST_AUTO_TEST_CASE(SendAndResendCountSeparately) {

    // The question this was reported as: "is there a swap?". There is not, and this is what says
    // so - each counter moves on its own and neither touches the other. A publish is not a resend
    // however many times the topic is replayed afterwards.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    repo.recordSend(kTopicErn, 10);
    repo.recordResend(kTopicErn, 4);

    const auto found = repo.findTopicByErn(kTopicErn);
    BOOST_REQUIRE(found.has_value());
    BOOST_TEST(found->send == 10L);
    BOOST_TEST(found->resend == 4L);
}

BOOST_AUTO_TEST_CASE(NeitherCounterMovesOnNothing) {

    // Guarded at the repository rather than the call site, so a publish loop that found no work
    // and a resend that released nothing both leave the totals where they were.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    repo.recordSend(kTopicErn, 0);
    repo.recordResend(kTopicErn, 0);
    repo.recordSend(kTopicErn, -5);

    const auto found = repo.findTopicByErn(kTopicErn);
    BOOST_REQUIRE(found.has_value());
    BOOST_TEST(found->send == 0L);
    BOOST_TEST(found->resend == 0L);
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

// ── Walking the topic without skip ──────────────────────────────────────────
//
// A resend hands over everything a topic holds, and it used to ask for page 0, page 1, page 2 and
// so on. Paging by index makes the database re-walk everything before the page it wants, so a
// whole-topic pass costs the square of the topic's size: measured on a 2.6-million-message topic
// at 2.5s for an early page and 7.3s two million in - hours of paging for one resend, getting
// worse as the topic grows. It now starts each page after the last message of the one before.
//
// What that has to preserve is everything below: every message, once each, in publish order,
// however the page boundaries fall.

BOOST_AUTO_TEST_CASE(TheWalkHandsOverEveryMessageExactlyOnce) {

    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    for (int i = 0; i < 25; ++i) {
        std::ignore = publish(repo, "m-" + std::to_string(i), "body-" + std::to_string(i));
    }

    // A page size that divides the topic unevenly, because that is where an off-by-one in the
    // cursor shows up rather than in the round case.
    const auto walked = walkInPages(repo, 7);

    BOOST_REQUIRE(walked.size() == 25U);
    for (int i = 0; i < 25; ++i) {
        BOOST_TEST(walked[static_cast<std::size_t>(i)] == "body-" + std::to_string(i));
    }
}

BOOST_AUTO_TEST_CASE(TheWalkAgreesWithTheWholeTopicWhateverThePageSize) {

    // The property: where the page boundaries fall must not change what comes out. A cursor that
    // repeats the boundary message, or steps over it, only shows up at some sizes.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    for (int i = 0; i < 20; ++i) {
        std::ignore = publish(repo, "m-" + std::to_string(i), "body-" + std::to_string(i));
    }

    const auto expected = bodiesInOrder(repo);
    BOOST_REQUIRE(expected.size() == 20U);

    for (const long pageSize: {1L, 2L, 3L, 19L, 20L, 21L, 500L}) {
        BOOST_TEST_CONTEXT("page size " << pageSize) {
            const bool same = walkInPages(repo, pageSize) == expected;
            BOOST_TEST(same);
        }
    }
}

BOOST_AUTO_TEST_CASE(MessagesPublishedInTheSameMomentAreAllHandedOver) {

    // Why the cursor is _id and not a timestamp. These are published as fast as the loop runs, so
    // they share a moment; a cursor on a timestamp would step over the rest of the moment the page
    // ended in - silently, and only for topics busy enough to put two messages in the same tick.
    // _id is unique, so there is no such moment to step over.
    //
    // ens_message has no "created" field at all, which is the other half of the same point: it is
    // never written, so a walk ordered by it is ordered by nothing.
    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    for (int i = 0; i < 6; ++i) {
        std::ignore = publish(repo, "same-" + std::to_string(i), "body-" + std::to_string(i));
    }

    // One per page, so every boundary falls inside the shared moment.
    BOOST_TEST(walkInPages(repo, 1).size() == 6U);
}

BOOST_AUTO_TEST_CASE(AnEmptyTopicWalksToNothing) {

    auto repo = freshRepository();
    std::ignore = topicOf(repo);

    BOOST_TEST(walkInPages(repo, 500).empty());
}

BOOST_AUTO_TEST_CASE(HeldMessagesStillMoveTheCursor) {

    // Held messages are passed over, not skipped: they are counted and not resent, but they are
    // still part of the order. Leaving one out of the cursor would start the next page on it
    // again, and the walk would never finish.
    auto repo = freshRepository();
    std::ignore = topicOf(repo, false);

    for (int i = 0; i < 5; ++i) {
        std::ignore = publish(repo, "h-" + std::to_string(i), "body-" + std::to_string(i));
    }

    // Every message is HELD, so a walk that only advanced past resent ones would loop for ever.
    const auto walked = walkInPages(repo, 2);
    BOOST_TEST(walked.size() == 5U);
}

