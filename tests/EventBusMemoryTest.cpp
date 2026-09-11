// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EventBusMemoryTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/EventBus.h>

using Euclid::Database::Database;
using Euclid::Database::EventBus;

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
