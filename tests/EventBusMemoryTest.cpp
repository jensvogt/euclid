// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EventBusMemoryTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <tuple>
#include <vector>

// Boost includes
#include <bsoncxx/builder/basic/document.hpp>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/EventBus.h>

using Euclid::Database::Database;
using Euclid::Database::EventBus;
using Euclid::Database::EventEnvelope;

// The event bus is the one part of euclid that had a second delivery mechanism: a change stream,
// which needs a replica set to tail an oplog and so cannot exist on the in-memory store. It was
// never the delivery mechanism though - it carries no payload and does nothing but call the poll
// sooner than the timer would - so what has to be shown here is that the poll path alone still
// delivers, because on that backend it is running alone.
//
// Everything it leans on is a place an emulated store is easy to get wrong: an atomic claim
// (find_one_and_update with a sort and the updated document back), a $or over a date, a unique
// index on the subscription, and an upsert that must not duplicate a subscription registered
// twice. So this exercises publish, claim and acknowledge end to end with no database anywhere.

namespace {

    constexpr auto kSubscriber = "invoice-service";
    constexpr auto kEventType = "esm.object.created";
    constexpr auto kAccount = "000000000000";

    boost::json::value payloadFor(const std::string &key) {
        boost::json::object payload;
        payload["accountId"] = kAccount;
        payload["region"] = "eu-central-1";
        payload["key"] = key;
        return payload;
    }

}// namespace

BOOST_AUTO_TEST_CASE(APublishedEventIsClaimedAndAcknowledged) {

    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    bus.SubscribeExternal(kSubscriber, kEventType, {}, kAccount);

    // Registering the same subscription again is an upsert, not a second subscriber: two rows
    // would mean two envelopes per event, and the consumer would process everything twice.
    bus.SubscribeExternal(kSubscriber, kEventType, {}, kAccount);

    bus.Publish(kEventType, payloadFor("first.txt"), "esm");
    bus.Publish(kEventType, payloadFor("second.txt"), "esm");

    BOOST_TEST(bus.CountEvents(kSubscriber) == 2);

    // Claimed oldest first, which is the sort the claim carries: an event stream delivered out of
    // order is worse than a slow one.
    const auto claimed = bus.ClaimEvents(kSubscriber, 10, std::chrono::seconds(30));
    BOOST_TEST_REQUIRE(claimed.size() == 2U);
    BOOST_TEST(claimed[0].eventType == kEventType);
    BOOST_TEST(claimed[0].sourceModule == "esm");
    BOOST_TEST(claimed[0].payload.at("key").as_string() == "first.txt");
    BOOST_TEST(claimed[1].payload.at("key").as_string() == "second.txt");

    // Claimed, not consumed: an event stays until it is acknowledged, so a consumer that dies
    // between the two loses nothing. What it must not do is come back on the next claim while the
    // visibility window is still open, or two instances would both process it.
    BOOST_TEST(bus.ClaimEvents(kSubscriber, 10, std::chrono::seconds(30)).empty());
    BOOST_TEST(bus.CountEvents(kSubscriber) == 2);

    BOOST_TEST(bus.AckEvent(kSubscriber, claimed[0].eventId));
    BOOST_TEST(bus.CountEvents(kSubscriber) == 1);

    // Acknowledging twice is not an error - a consumer that retries an ack after a timeout must
    // not be told something went wrong.
    BOOST_TEST(!bus.AckEvent(kSubscriber, claimed[0].eventId));

    BOOST_TEST(bus.AckEvent(kSubscriber, claimed[1].eventId));
    BOOST_TEST(bus.CountEvents(kSubscriber) == 0);
}

BOOST_AUTO_TEST_CASE(AnEventForAnotherAccountIsNeverStored) {

    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    bus.SubscribeExternal(kSubscriber, kEventType, {}, kAccount);

    boost::json::object elsewhere;
    elsewhere["accountId"] = "999999999999";
    elsewhere["key"] = "not-mine.txt";
    bus.Publish(kEventType, elsewhere, "esm");

    BOOST_TEST(bus.CountEvents(kSubscriber) == 0);
    BOOST_TEST(bus.ClaimEvents(kSubscriber, 10, std::chrono::seconds(30)).empty());
}

BOOST_AUTO_TEST_CASE(UnsubscribingTakesTheBacklogWithIt) {

    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    bus.SubscribeExternal(kSubscriber, kEventType, {}, kAccount);
    bus.Publish(kEventType, payloadFor("first.txt"), "esm");
    BOOST_TEST_REQUIRE(bus.CountEvents(kSubscriber) == 1);

    // The events go with the subscription. The store has no TTL index to expire them later, so if
    // unsubscribing left them they would stay for the life of the process.
    BOOST_TEST(bus.UnsubscribeExternal(kSubscriber, kEventType) == 1);
    BOOST_TEST(bus.CountEvents(kSubscriber) == 0);
    BOOST_TEST(bus.ListSubscriptions(kSubscriber).empty());
}

// ── A batch is the same events, written together ───────────────────────────
//
// A purge publishes one esm.object.deleted per removed object, because a subscriber keeping an
// index of keys has to be told which ones went. Doing that an insert at a time made the announcing
// cost more than the deleting: on a loaded installation 1,000 envelopes measured 13,621 ms one at a
// time against 29 ms together. What must NOT change is what a subscriber receives, which is what
// these pin.

BOOST_AUTO_TEST_CASE(ABatchDeliversTheSameEnvelopesAsOneByOne) {

    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();
    bus.SubscribeExternal(kSubscriber, kEventType, {}, kAccount);

    bus.PublishBatch(kEventType,
                     {{payloadFor("first.txt"), {}}, {payloadFor("second.txt"), {}}, {payloadFor("third.txt"), {}}},
                     "esm");

    BOOST_TEST(bus.CountEvents(kSubscriber) == 3);

    // Order is the batch's order. A stream delivered out of order is worse than a slow one, and a
    // batch must not become the place that reorders it.
    const auto claimed = bus.ClaimEvents(kSubscriber, 10, std::chrono::seconds(30));
    BOOST_TEST_REQUIRE(claimed.size() == 3U);
    BOOST_TEST(claimed[0].payload.at("key").as_string() == "first.txt");
    BOOST_TEST(claimed[1].payload.at("key").as_string() == "second.txt");
    BOOST_TEST(claimed[2].payload.at("key").as_string() == "third.txt");
    BOOST_TEST(claimed[0].eventType == kEventType);
    BOOST_TEST(claimed[0].sourceModule == "esm");
}

BOOST_AUTO_TEST_CASE(AnEmptyBatchPublishesNothing) {

    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();
    bus.SubscribeExternal(kSubscriber, kEventType, {}, kAccount);

    bus.PublishBatch(kEventType, {}, "esm");

    BOOST_TEST(bus.CountEvents(kSubscriber) == 0);
}

BOOST_AUTO_TEST_CASE(ABatchWithNoSubscribersStoresNothing) {

    // The early return matters at this size: a million-object purge of a bucket nobody watches
    // must not build a million envelopes for nobody.
    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    bus.PublishBatch("esm.object.deleted", {{payloadFor("gone.txt"), {}}}, "esm");

    BOOST_TEST(bus.CountEvents(kSubscriber) == 0);
}

BOOST_AUTO_TEST_CASE(AccountScopingIsStillPerEventInABatch) {

    // The one thing a batch could plausibly get wrong: the subscriber list is resolved once, but
    // the filter and the account check are properties of each payload. Resolving those once too
    // would send one account's events to another account's subscriber.
    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();
    bus.SubscribeExternal(kSubscriber, kEventType, {}, kAccount);

    boost::json::object other = payloadFor("theirs.txt").as_object();
    other["accountId"] = "999999999999";

    bus.PublishBatch(kEventType, {{payloadFor("ours.txt"), {}}, {boost::json::value(other), {}}}, "esm");

    BOOST_TEST(bus.CountEvents(kSubscriber) == 1);
    const auto claimed = bus.ClaimEvents(kSubscriber, 10, std::chrono::seconds(30));
    BOOST_TEST_REQUIRE(claimed.size() == 1U);
    BOOST_TEST(claimed[0].payload.at("key").as_string() == "ours.txt");
}

BOOST_AUTO_TEST_CASE(AFilterIsStillAppliedPerEventInABatch) {

    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();
    bus.SubscribeExternal(kSubscriber, kEventType, {{"region", boost::json::value("eu-central-1")}}, kAccount);

    boost::json::object elsewhere = payloadFor("far.txt").as_object();
    elsewhere["region"] = "us-east-1";

    bus.PublishBatch(kEventType, {{payloadFor("near.txt"), {}}, {boost::json::value(elsewhere), {}}}, "esm");

    BOOST_TEST(bus.CountEvents(kSubscriber) == 1);
}

// ── Module delivery, claimed and settled a batch at a time ─────────────────
//
// pollOnce() had no coverage while it was private, and it is the path that decides whether a
// module event is delivered once, delivered twice, or quietly lost. It also used to be the reason
// a backlog could not be drained: a find_one_and_update per event and a delete per ack, two
// synchronous round trips each, so a consumer emptied the collection at roughly 37 events a second
// however many were waiting. An ESM purge filled it faster than that and the remainder had to be
// deleted by hand.

namespace {

    long pendingFor(const std::string &moduleType) {
        long pending = 0;
        for (const auto &doc: Database::instance().collection("ees_events").find(
                     bsoncxx::builder::basic::make_document(
                             bsoncxx::builder::basic::kvp("targetModule", moduleType)))) {
            std::ignore = doc;
            ++pending;
        }
        return pending;
    }

}// namespace

BOOST_AUTO_TEST_CASE(OneBatchDeliversEveryPendingEvent) {

    // The point of the change. Twenty events used to need twenty claims and twenty deletes; they
    // are now one pass, and the pass has to actually deliver all of them.
    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    std::vector<std::string> seen;
    bus.Subscribe("eqs", kEventType, [&seen](const EventEnvelope &envelope) {
        seen.push_back(std::string(envelope.payload.at("key").as_string()));
        return true;
    });

    for (int i = 0; i < 20; ++i) bus.Publish(kEventType, payloadFor("file-" + std::to_string(i) + ".txt"), "esm");
    BOOST_TEST_REQUIRE(pendingFor("eqs") == 20L);

    bus.pollOnce("eqs");

    BOOST_TEST(seen.size() == 20U);
    BOOST_TEST(pendingFor("eqs") == 0L);
}

BOOST_AUTO_TEST_CASE(ADeliveredEventIsRemovedAndNotDeliveredAgain) {

    // The ack half. Settling the batch is a single delete_many now, and an event it misses would
    // be handed to the handler a second time on the next pass - a duplicate, not a loss, which is
    // the harder kind to notice.
    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    long calls = 0;
    bus.Subscribe("eqs", kEventType, [&calls](const EventEnvelope &) {
        ++calls;
        return true;
    });

    bus.Publish(kEventType, payloadFor("once.txt"), "esm");

    bus.pollOnce("eqs");
    bus.pollOnce("eqs");
    bus.pollOnce("eqs");

    BOOST_TEST(calls == 1L);
    BOOST_TEST(pendingFor("eqs") == 0L);
}

BOOST_AUTO_TEST_CASE(EventsAreDeliveredOldestFirst) {

    // The claim sorts by createdAt, and batching must not become the place that reorders a stream.
    // Out of order is worse than slow for a consumer keeping an index.
    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    std::vector<std::string> seen;
    bus.Subscribe("eqs", kEventType, [&seen](const EventEnvelope &envelope) {
        seen.push_back(std::string(envelope.payload.at("key").as_string()));
        return true;
    });

    bus.Publish(kEventType, payloadFor("first.txt"), "esm");
    bus.Publish(kEventType, payloadFor("second.txt"), "esm");
    bus.Publish(kEventType, payloadFor("third.txt"), "esm");

    bus.pollOnce("eqs");

    BOOST_TEST_REQUIRE(seen.size() == 3U);
    BOOST_TEST(seen[0] == "first.txt");
    BOOST_TEST(seen[1] == "second.txt");
    BOOST_TEST(seen[2] == "third.txt");
}

BOOST_AUTO_TEST_CASE(AHandlerThatRefusesKeepsTheEventForAnotherTry) {

    // A batch settles in two statements - one delete for what was handled, one requeue for what
    // was not - and putting an event in the wrong one either loses it or replays it forever. The
    // refused event has to come back, and the accepted one beside it has to not.
    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    long attempts = 0;
    bus.Subscribe("eqs", kEventType, [&attempts](const EventEnvelope &envelope) {
        ++attempts;
        return std::string(envelope.payload.at("key").as_string()) != "stubborn.txt";
    });

    bus.Publish(kEventType, payloadFor("easy.txt"), "esm");
    bus.Publish(kEventType, payloadFor("stubborn.txt"), "esm");

    bus.pollOnce("eqs");
    BOOST_TEST(attempts == 2L);
    BOOST_TEST(pendingFor("eqs") == 1L);

    bus.pollOnce("eqs");
    BOOST_TEST(attempts == 3L);
    BOOST_TEST(pendingFor("eqs") == 1L);
}

BOOST_AUTO_TEST_CASE(NothingPendingIsNotAnError) {

    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    long calls = 0;
    bus.Subscribe("eqs", kEventType, [&calls](const EventEnvelope &) {
        ++calls;
        return true;
    });

    bus.pollOnce("eqs");

    BOOST_TEST(calls == 0L);
}

BOOST_AUTO_TEST_CASE(OneModulesEventsAreNotDeliveredToAnother) {

    // The batch is selected by targetModule, and a $in over ids is a wider net than a
    // find_one_and_update was - so it is worth saying that the net is still cast per module.
    Database::instance().initializeMemory();
    auto &bus = EventBus::instance();

    long eqsCalls = 0;
    bus.Subscribe("eqs", kEventType, [&eqsCalls](const EventEnvelope &) {
        ++eqsCalls;
        return true;
    });
    bus.Subscribe("ens", "ens.message.published", [](const EventEnvelope &) { return true; });

    bus.Publish(kEventType, payloadFor("for-eqs.txt"), "esm");

    bus.pollOnce("ens");
    BOOST_TEST(eqsCalls == 0L);
    BOOST_TEST(pendingFor("eqs") == 1L);

    bus.pollOnce("eqs");
    BOOST_TEST(eqsCalls == 1L);
}
