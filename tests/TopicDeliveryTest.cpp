#define BOOST_TEST_MODULE TopicDeliveryTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>
#include <thread>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>

using Euclid::Database::MongoEnsRepository;
using Euclid::Database::Entity::ENS::kStatusHeld;
using Euclid::Database::Entity::ENS::kStatusPublished;

// Stopping a topic holds what is published to it instead of handing it to the subscribers, and
// starting it again hands over what was held. What this pins down is the storage half of that: the
// status a publish is given, which messages a start finds and in what order, and what happens to
// them afterwards. The fan-out itself belongs to the module - it publishes on the event bus - and
// is not something a repository test can see.

namespace {

    MongoEnsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEnsRepository{};
    }

    std::string topicErn(const std::string &name) {
        return "ern:ens:eu-central-1:000000000000:development:topic:" + name;
    }

    void addTopic(MongoEnsRepository &repo, const std::string &ern, const bool delivering) {
        Euclid::Database::Entity::ENS::Topic topic;
        topic.ern = ern;
        topic.name = ern.substr(ern.rfind(':') + 1);
        topic.delivering = delivering;
        repo.upsertTopic(topic);
    }

    Euclid::Database::Entity::ENS::Message publish(MongoEnsRepository &repo, const std::string &ern,
                                                   const std::string &messageId, const std::string &priority = "MIDDLE") {
        return repo.publishMessage(messageId, "ern:ens:eu-central-1:000000000000:message:" + messageId,
                                   ern, R"({"id":")" + messageId + R"("})", {}, priority);
    }

}// namespace

BOOST_AUTO_TEST_CASE(ATopicDeliversUnlessItIsStopped) {

    // The default, and what a topic written before this existed reads back as: a document with no
    // such field must not come back stopped.
    const Euclid::Database::Entity::ENS::Topic fresh;
    BOOST_TEST(fresh.delivering);

    Euclid::Database::Entity::ENS::Topic topic;
    topic.ern = topicErn("legacy");
    topic.name = "legacy";
    auto document = topic.toDocument();

    BOOST_TEST(Euclid::Database::Entity::ENS::Topic::fromDocument(document.view()).delivering);
}

BOOST_AUTO_TEST_CASE(TheStatusReadsFromWhetherItIsDelivering) {

    // What a listing and get-topic-metadata report. Derived rather than stored, so there is no
    // second copy of this to disagree with the flag.
    Euclid::Database::Entity::ENS::Topic topic;
    BOOST_TEST(topic.status() == "RUNNING");

    topic.delivering = false;
    BOOST_TEST(topic.status() == "STOPPED");
}

BOOST_AUTO_TEST_CASE(TheStatusSurvivesTheDatabase) {

    auto repo = freshRepository();
    const auto ern = topicErn("reported");
    addTopic(repo, ern, false);

    BOOST_TEST(repo.findTopicByErn(ern).value().status() == "STOPPED");

    addTopic(repo, ern, true);
    BOOST_TEST(repo.findTopicByErn(ern).value().status() == "RUNNING");
}

BOOST_AUTO_TEST_CASE(HeldMessagesAreCountedWithoutReadingThem) {

    auto repo = freshRepository();
    const auto ern = topicErn("countable");
    addTopic(repo, ern, false);

    BOOST_TEST(repo.countHeldMessages(ern) == 0);

    publish(repo, ern, "one");
    publish(repo, ern, "two");
    BOOST_TEST(repo.countHeldMessages(ern) == 2);

    repo.markMessageDelivered("one");
    BOOST_TEST(repo.countHeldMessages(ern) == 1);
}

BOOST_AUTO_TEST_CASE(APublishToADeliveringTopicIsPublished) {

    auto repo = freshRepository();
    const auto ern = topicErn("running");
    addTopic(repo, ern, true);

    BOOST_TEST(publish(repo, ern, "msg-1").status == kStatusPublished);
    BOOST_TEST(repo.listHeldMessages(ern, 10).empty());
}

BOOST_AUTO_TEST_CASE(APublishToAStoppedTopicIsHeld) {

    // Stored, not refused and not dropped: holding what arrives is the whole reason to stop a
    // topic rather than to unsubscribe from it.
    auto repo = freshRepository();
    const auto ern = topicErn("stopped");
    addTopic(repo, ern, false);

    const auto message = publish(repo, ern, "msg-held");
    BOOST_TEST(message.status == kStatusHeld);

    const auto held = repo.listHeldMessages(ern, 10);
    BOOST_REQUIRE(held.size() == 1);
    BOOST_TEST(held.front().messageId == "msg-held");
    BOOST_TEST(held.front().body == R"({"id":"msg-held"})");
}

BOOST_AUTO_TEST_CASE(AHeldMessageKeepsThePriorityItWasPublishedWith) {

    // A replay that dropped this would quietly turn urgent work into ordinary work when it reached
    // the queue.
    auto repo = freshRepository();
    const auto ern = topicErn("priorities");
    addTopic(repo, ern, false);

    publish(repo, ern, "msg-high", "HIGH");

    const auto held = repo.listHeldMessages(ern, 10);
    BOOST_REQUIRE(held.size() == 1);
    BOOST_TEST(held.front().priority == "HIGH");
}

BOOST_AUTO_TEST_CASE(HeldMessagesComeBackOldestFirst) {

    auto repo = freshRepository();
    const auto ern = topicErn("ordered");
    addTopic(repo, ern, false);

    // Spaced out, because the order is the order they were published in and a stored date has
    // millisecond resolution.
    for (const auto &id: {"first", "second", "third"}) {
        publish(repo, ern, id);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto held = repo.listHeldMessages(ern, 10);
    BOOST_REQUIRE(held.size() == 3);
    BOOST_TEST(held[0].messageId == "first");
    BOOST_TEST(held[1].messageId == "second");
    BOOST_TEST(held[2].messageId == "third");
}

BOOST_AUTO_TEST_CASE(AStartReadsTheBacklogInPages) {

    auto repo = freshRepository();
    const auto ern = topicErn("paged");
    addTopic(repo, ern, false);

    for (const auto &id: {"a", "b", "c"}) {
        publish(repo, ern, id);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // What the module does with a backlog too large to hold in memory: read a page, deliver it,
    // mark it, and come back for the next one.
    BOOST_TEST(repo.listHeldMessages(ern, 2).size() == 2);

    repo.markMessageDelivered("a");
    repo.markMessageDelivered("b");

    const auto remaining = repo.listHeldMessages(ern, 2);
    BOOST_REQUIRE(remaining.size() == 1);
    BOOST_TEST(remaining.front().messageId == "c");
}

BOOST_AUTO_TEST_CASE(AMessageMarkedDeliveredIsNoLongerHeld) {

    auto repo = freshRepository();
    const auto ern = topicErn("delivered");
    addTopic(repo, ern, false);
    publish(repo, ern, "msg-mark");

    repo.markMessageDelivered("msg-mark");

    BOOST_TEST(repo.listHeldMessages(ern, 10).empty());

    const auto read = repo.findMessageById("msg-mark");
    BOOST_REQUIRE(read.has_value());
    BOOST_TEST(read->status == kStatusPublished);
}

BOOST_AUTO_TEST_CASE(OneTopicsBacklogIsNotAnothers) {

    auto repo = freshRepository();
    const auto mine = topicErn("mine");
    const auto yours = topicErn("yours");
    addTopic(repo, mine, false);
    addTopic(repo, yours, false);

    publish(repo, mine, "msg-mine");
    publish(repo, yours, "msg-yours");

    const auto held = repo.listHeldMessages(mine, 10);
    BOOST_REQUIRE(held.size() == 1);
    BOOST_TEST(held.front().messageId == "msg-mine");
}

BOOST_AUTO_TEST_CASE(ReleasedMessagesAreCountedOnTheTopic) {

    auto repo = freshRepository();
    const auto ern = topicErn("counted");
    addTopic(repo, ern, false);

    repo.recordResend(ern, 3);
    BOOST_TEST(repo.findTopicByErn(ern).value().resend == 3);

    // Added to rather than replaced, so a second start counts on from the first.
    repo.recordResend(ern, 2);
    BOOST_TEST(repo.findTopicByErn(ern).value().resend == 5);

    // Nothing released is nothing to count, and no write to make.
    repo.recordResend(ern, 0);
    BOOST_TEST(repo.findTopicByErn(ern).value().resend == 5);
}
