// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE UpdateMessageBodyTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/ContentTypeUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>
#include <euclid/dto/ens/UpdateMessageBodyRequest.h>
#include <euclid/dto/eqs/UpdateMessageBodyRequest.h>

using Euclid::Database::MongoEnsRepository;
using Euclid::Database::MongoEqsRepository;
using Euclid::Database::Entity::EQS::MessagePriority;

// Rewriting a message body is three writes that have to stay in step, not one. The body itself is
// the obvious part; the two that are easy to leave behind are Message::size - which is what the
// queue's or topic's stored byte total is made of - and contentType, which was derived from the
// body when the message was sent and otherwise goes on describing what the message used to hold.
//
// The failure these are mostly about is silent and permanent: a queue whose byte total counts a
// body that is no longer there. Nothing refuses, nothing logs, and the only way back is the recount
// job. So the arithmetic is checked against the queue row rather than against the message.

namespace {

    constexpr auto kQueue = "ern:eqs:eu-central-1:000000000000:production:queue:orders";
    constexpr auto kTopic = "ern:ens:eu-central-1:000000000000:production:topic:order-events";

    MongoEqsRepository freshQueueRepository() {
        // Counter adjustments are coalesced in state that outlives a repository instance, so a test
        // that did not flush leaves its deltas behind for whoever flushes next - which would be the
        // next test, against its own fresh queue. Drained here into the store they belong to,
        // before that store is replaced.
        //
        // Not on the first call: there is no previous test to have left anything, and constructing
        // a repository before the store exists logs an index failure that reads like a fault.
        static bool aTestHasRun = false;
        if (aTestHasRun) MongoEqsRepository{}.flushQueueCounters();
        aTestHasRun = true;
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEqsRepository{};
    }

    Euclid::Database::Entity::EQS::Queue queueOf(MongoEqsRepository &repo) {
        Euclid::Database::Entity::EQS::Queue queue;
        queue.name = "orders";
        queue.ern = kQueue;
        queue.accountId = "000000000000";
        queue.nameSpace = "production";
        queue.region = "eu-central-1";
        queue.visibility = 30;
        queue.maxReceiveCount = 3;
        return repo.upsertQueue(queue);
    }

    long storedQueueSize(const MongoEqsRepository &repo) {
        const auto queue = repo.findQueueByErn(kQueue);
        BOOST_TEST_REQUIRE(queue.has_value());
        return queue->size;
    }

}// namespace

// ── EQS ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheBodyIsReplacedWholesale) {

    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "the original", {}, {}, MessagePriority::MEDIUM);

    const auto updated = repo.updateMessageBody("m-1", "something else entirely");

    BOOST_TEST_REQUIRE(updated.has_value());
    BOOST_TEST(updated->body == "something else entirely");
}

BOOST_AUTO_TEST_CASE(TheSizeFollowsTheNewBody) {

    // Not the length of what it replaced, which is what writing the body alone would leave behind.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);

    const auto updated = repo.updateMessageBody("m-1", "0123456789012345678901234");

    BOOST_TEST_REQUIRE(updated.has_value());
    BOOST_TEST(updated->size == 25L);
}

BOOST_AUTO_TEST_CASE(TheQueuesByteTotalMovesByTheDifference) {

    // The one that goes wrong invisibly. A queue counting a body that is no longer there is not
    // refused by anything and is not logged - it is simply wrong until the recount job runs.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();
    BOOST_TEST_REQUIRE(storedQueueSize(repo) == 10L);

    std::ignore = repo.updateMessageBody("m-1", "0123456789012345678901234");
    repo.flushQueueCounters();

    BOOST_TEST(storedQueueSize(repo) == 25L);
}

BOOST_AUTO_TEST_CASE(AShorterBodyTakesTheTotalDownAgain) {

    // The other direction, because a delta applied with the wrong sign passes the growing case.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789012345678901234", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();
    BOOST_TEST_REQUIRE(storedQueueSize(repo) == 25L);

    std::ignore = repo.updateMessageBody("m-1", "0123456789");
    repo.flushQueueCounters();

    BOOST_TEST(storedQueueSize(repo) == 10L);
}

BOOST_AUTO_TEST_CASE(ASameLengthCorrectionLeavesTheTotalAlone) {

    // The common case for a correction - a typo fixed, a field respelled - and the one where an
    // adjustment that ran unconditionally would show up as drift rather than as a wrong number.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "aaaaaaaaaa", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();

    std::ignore = repo.updateMessageBody("m-1", "bbbbbbbbbb");
    repo.flushQueueCounters();

    BOOST_TEST(storedQueueSize(repo) == 10L);
}

BOOST_AUTO_TEST_CASE(AnEmptyBodyIsAcceptedAndCountsAsNothing) {

    // send-message accepts an empty body, so refusing to update one to the shape it could have been
    // sent as would be a rule that applies only the second time.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();

    const auto updated = repo.updateMessageBody("m-1", "");
    repo.flushQueueCounters();

    BOOST_TEST_REQUIRE(updated.has_value());
    BOOST_TEST(updated->body.empty());
    BOOST_TEST(updated->size == 0L);
    BOOST_TEST(storedQueueSize(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(TheContentTypeIsDerivedFromTheNewBody) {

    // A stored content type describing what the message used to hold is worse than none at all,
    // because it is believed. Checked against the derivation rather than against "it changed":
    // fromContent() answers through libmagic, which is not loaded everywhere this test runs, and
    // where it is missing every body is application/octet-stream - so a test asserting that two
    // content types differ would pass or fail on the environment rather than on the code.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, R"({"order":1})", {}, {}, MessagePriority::MEDIUM);

    static constexpr auto kNewBody = "just words now";
    const auto updated = repo.updateMessageBody("m-1", kNewBody);

    BOOST_TEST_REQUIRE(updated.has_value());
    BOOST_TEST(updated->contentType == Euclid::Core::ContentTypeUtils::fromContent(kNewBody),
               "the content type was not derived from the body that is now stored: " + updated->contentType);
}

BOOST_AUTO_TEST_CASE(WhatTheBodyDoesNotDecideIsLeftAlone) {

    // A body correction is not a redelivery. The identifiers, the routing and the attributes are
    // not the body's to change, and a message in flight stays in flight.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "the original", {}, {}, MessagePriority::HIGH);

    const auto before = repo.findMessageByName("m-1");
    BOOST_TEST_REQUIRE(before.has_value());

    const auto updated = repo.updateMessageBody("m-1", "corrected");

    BOOST_TEST_REQUIRE(updated.has_value());
    BOOST_TEST(updated->messageId == before->messageId);
    BOOST_TEST(updated->queueErn == before->queueErn);
    BOOST_TEST(updated->ern == before->ern);
    BOOST_TEST(updated->receiptHandle == before->receiptHandle);
    BOOST_TEST((updated->status == before->status));
    BOOST_TEST((updated->priority == before->priority));
    BOOST_TEST(updated->receivedCount == before->receivedCount);
    BOOST_TEST(updated->visibilityTimeout == before->visibilityTimeout);
}

BOOST_AUTO_TEST_CASE(AMessageThatIsNotThereAnswersNothing) {

    // What the handler turns into a 404 rather than reporting a write that did not happen.
    auto repo = freshQueueRepository();
    queueOf(repo);

    BOOST_TEST(!repo.updateMessageBody("no-such-message", "anything").has_value());
}

BOOST_AUTO_TEST_CASE(AMissingMessageLeavesTheQueuesTotalAlone) {

    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();

    std::ignore = repo.updateMessageBody("no-such-message", "0123456789012345678901234");
    repo.flushQueueCounters();

    BOOST_TEST(storedQueueSize(repo) == 10L);
}

BOOST_AUTO_TEST_CASE(RewritingTwiceCountsFromTheBodyItActuallyReplaced) {

    // The arithmetic is a delta against what is stored, not against what was originally sent - so a
    // second rewrite has to measure from the first one's result.
    auto repo = freshQueueRepository();
    queueOf(repo);
    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();

    std::ignore = repo.updateMessageBody("m-1", "01234567890123456789");
    repo.flushQueueCounters();
    BOOST_TEST_REQUIRE(storedQueueSize(repo) == 20L);

    std::ignore = repo.updateMessageBody("m-1", "012");
    repo.flushQueueCounters();

    BOOST_TEST(storedQueueSize(repo) == 3L);
}

// ── ENS ─────────────────────────────────────────────────────────────────────

namespace {

    MongoEnsRepository freshTopicRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEnsRepository{};
    }

    void topicOf(MongoEnsRepository &repo) {
        Euclid::Database::Entity::ENS::Topic topic;
        topic.name = "order-events";
        topic.ern = kTopic;
        topic.accountId = "000000000000";
        topic.nameSpace = "production";
        topic.region = "eu-central-1";
        topic.delivering = true;
        std::ignore = repo.upsertTopic(topic);
    }

    long storedTopicSize(const MongoEnsRepository &repo) {
        const auto topic = repo.findTopicByErn(kTopic);
        BOOST_TEST_REQUIRE(topic.has_value());
        return topic->size;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ATopicMessageBodyIsReplacedWholesale) {

    auto repo = freshTopicRepository();
    topicOf(repo);
    std::ignore = repo.publishMessage("t-1", "ern:ens:...:message:t-1", kTopic, "the original", {}, "MEDIUM");

    const auto updated = repo.updateMessageBody("t-1", "something else entirely");

    BOOST_TEST_REQUIRE(updated.has_value());
    BOOST_TEST(updated->body == "something else entirely");
    BOOST_TEST(updated->size == 23L);
}

BOOST_AUTO_TEST_CASE(TheTopicsByteTotalMovesByTheDifference) {

    auto repo = freshTopicRepository();
    topicOf(repo);
    std::ignore = repo.publishMessage("t-1", "ern:ens:...:message:t-1", kTopic, "0123456789", {}, "MEDIUM");
    BOOST_TEST_REQUIRE(storedTopicSize(repo) == 10L);

    std::ignore = repo.updateMessageBody("t-1", "0123456789012345678901234");

    BOOST_TEST(storedTopicSize(repo) == 25L);
}

BOOST_AUTO_TEST_CASE(AShorterTopicBodyTakesTheTotalDownAgain) {

    auto repo = freshTopicRepository();
    topicOf(repo);
    std::ignore = repo.publishMessage("t-1", "ern:ens:...:message:t-1", kTopic, "0123456789012345678901234", {}, "MEDIUM");
    BOOST_TEST_REQUIRE(storedTopicSize(repo) == 25L);

    std::ignore = repo.updateMessageBody("t-1", "0123456789");

    BOOST_TEST(storedTopicSize(repo) == 10L);
}

BOOST_AUTO_TEST_CASE(RewritingATopicMessageDoesNotRepublishIt) {

    // `available` is what the topic says it is holding. Rewriting a body neither publishes nor
    // consumes anything, so it has to leave that number exactly where it was - an $inc that took
    // the whole size rather than the difference would be visible here as well.
    auto repo = freshTopicRepository();
    topicOf(repo);
    std::ignore = repo.publishMessage("t-1", "ern:ens:...:message:t-1", kTopic, "0123456789", {}, "MEDIUM");

    const auto before = repo.findTopicByErn(kTopic);
    BOOST_TEST_REQUIRE(before.has_value());

    std::ignore = repo.updateMessageBody("t-1", "0123456789012345678901234");

    const auto after = repo.findTopicByErn(kTopic);
    BOOST_TEST_REQUIRE(after.has_value());
    BOOST_TEST(after->available == before->available);
}

BOOST_AUTO_TEST_CASE(ATopicMessageThatIsNotThereAnswersNothing) {

    auto repo = freshTopicRepository();
    topicOf(repo);

    BOOST_TEST(!repo.updateMessageBody("no-such-message", "anything").has_value());
}

// ── The request as it arrives ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheQueueRequestReadsBothFields) {

    const auto request = Euclid::Dto::EQS::UpdateMessageBodyRequest::fromJson(R"({"messageId":"m-1","body":"corrected"})");

    BOOST_TEST(request.messageId == "m-1");
    BOOST_TEST(request.body == "corrected");
}

BOOST_AUTO_TEST_CASE(TheTopicRequestReadsBothFields) {

    const auto request = Euclid::Dto::ENS::UpdateMessageBodyRequest::fromJson(R"({"messageId":"t-1","body":"corrected"})");

    BOOST_TEST(request.messageId == "t-1");
    BOOST_TEST(request.body == "corrected");
}

BOOST_AUTO_TEST_CASE(AnAbsentBodyReadsAsEmptyRatherThanThrowing) {

    // Which the handler then stores as an empty body. The field is required in practice by the CLI
    // and by anything that means to change something, but a missing one is not a parse failure -
    // "body": "" and no body at all are the same request here.
    const auto request = Euclid::Dto::EQS::UpdateMessageBodyRequest::fromJson(R"({"messageId":"m-1"})");

    BOOST_TEST(request.messageId == "m-1");
    BOOST_TEST(request.body.empty());
}

BOOST_AUTO_TEST_CASE(TheRequestsRoundTrip) {

    Euclid::Dto::EQS::UpdateMessageBodyRequest queueRequest;
    queueRequest.messageId = "m-1";
    queueRequest.body = R"({"order":1})";

    const auto queueBack = Euclid::Dto::EQS::UpdateMessageBodyRequest::fromJson(queueRequest.toJson());
    BOOST_TEST(queueBack.messageId == queueRequest.messageId);
    BOOST_TEST(queueBack.body == queueRequest.body);

    Euclid::Dto::ENS::UpdateMessageBodyRequest topicRequest;
    topicRequest.messageId = "t-1";
    topicRequest.body = R"({"order":1})";

    const auto topicBack = Euclid::Dto::ENS::UpdateMessageBodyRequest::fromJson(topicRequest.toJson());
    BOOST_TEST(topicBack.messageId == topicRequest.messageId);
    BOOST_TEST(topicBack.body == topicRequest.body);
}
