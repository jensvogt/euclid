#define BOOST_TEST_MODULE EventBusExternalTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>
#include <thread>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/EventBus.h>

using Euclid::Core::Configuration;
using Euclid::Core::UuidUtils;
using Euclid::Database::EventBus;

// External subscribers are the durable half of client event delivery: an application registers a
// name, events are stored for it whether or not it is connected, and one is removed only when it
// says it is done with it. That behaviour lives in MongoDB, so this exercises it there rather
// than against a stub - the claim, the lease and the fan-out are database operations, and a fake
// would prove nothing about them.
//
// It runs against a database of its own ("euclid_test"), under names generated per run, and
// removes what it created. With no MongoDB reachable it reports that and passes, so it stays
// usable in a build that has no database.

namespace {

    bool databaseAvailable() {
        static const bool available = [] {
            Configuration::instance().set<std::string>("euclid.mongodb.name", "euclid_test");
            try {
                Euclid::Database::Database::instance().initialize();
                const auto entry = Euclid::Database::Database::instance().client();
                (*entry)["euclid_test"].run_command(bsoncxx::builder::basic::make_document(
                        bsoncxx::builder::basic::kvp("ping", 1)));
                return true;
            } catch (const std::exception &e) {
                BOOST_TEST_MESSAGE("no MongoDB reachable, skipping event bus integration tests: " << e.what());
                return false;
            }
        }();
        return available;
    }

    struct Fixture {
        std::string subscriber = "test-" + UuidUtils::CreateRandomUuid();
        std::string eventType = "test.event." + UuidUtils::CreateRandomUuid();

        ~Fixture() {
            if (databaseAvailable()) EventBus::instance().UnsubscribeExternal(subscriber, "");
        }
    };

}// namespace

BOOST_AUTO_TEST_CASE(EventsWaitForASubscriberThatIsNotConnected) {
    if (!databaseAvailable()) return;
    Fixture fixture;

    auto &bus = EventBus::instance();
    bus.SubscribeExternal(fixture.subscriber, fixture.eventType, {}, "000000000000");

    // Published with nobody listening on a socket: the point of the durable path is that the
    // event is kept until somebody asks for it.
    bus.Publish(fixture.eventType, boost::json::value{{"key", "report.csv"}, {"accountId", "000000000000"}}, "test");
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 1);

    const auto claimed = bus.ClaimEvents(fixture.subscriber, 10, std::chrono::seconds(60));
    BOOST_TEST_REQUIRE(claimed.size() == 1U);
    BOOST_TEST(claimed[0].eventType == fixture.eventType);
    BOOST_TEST(claimed[0].payload.at("key").as_string() == "report.csv");
    BOOST_TEST(claimed[0].attempts == 1);

    // A claim is a lease, not a delete: the event is still there until it is acknowledged, which
    // is what makes a consumer crashing mid-work harmless.
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 1);
    BOOST_TEST(bus.AckEvent(fixture.subscriber, claimed[0].eventId));
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 0);
}

BOOST_AUTO_TEST_CASE(AClaimedEventIsInvisibleToTheNextClaimer) {
    if (!databaseAvailable()) return;
    Fixture fixture;

    auto &bus = EventBus::instance();
    bus.SubscribeExternal(fixture.subscriber, fixture.eventType, {}, "000000000000");
    bus.Publish(fixture.eventType, boost::json::value{{"n", 1}}, "test");

    // Two instances of one application share a subscriber name; exactly one gets the event. The
    // lease is deliberately short here, because the second half of this test is what happens when
    // it runs out.
    const auto first = bus.ClaimEvents(fixture.subscriber, 10, std::chrono::seconds(1));
    const auto second = bus.ClaimEvents(fixture.subscriber, 10, std::chrono::seconds(1));
    BOOST_TEST(first.size() == 1U);
    BOOST_TEST(second.empty());

    // And it comes back once that lease expires, rather than being lost with the instance that
    // took it and never acknowledged it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto afterLease = bus.ClaimEvents(fixture.subscriber, 10, std::chrono::seconds(60));
    BOOST_TEST_REQUIRE(afterLease.size() == 1U);
    BOOST_TEST(afterLease[0].attempts == 2);
}

BOOST_AUTO_TEST_CASE(TwoSubscribersEachGetTheirOwnCopy) {
    if (!databaseAvailable()) return;
    Fixture one;
    Fixture two;
    two.eventType = one.eventType;// two applications watching the same thing

    auto &bus = EventBus::instance();
    bus.SubscribeExternal(one.subscriber, one.eventType, {}, "000000000000");
    bus.SubscribeExternal(two.subscriber, one.eventType, {}, "000000000000");

    bus.Publish(one.eventType, boost::json::value{{"key", "shared.csv"}}, "test");

    // This is what makes a queue per consumer unnecessary: the fan-out is per subscriber, so one
    // acknowledging its copy does not take the event away from the other.
    BOOST_TEST(bus.CountEvents(one.subscriber) == 1);
    BOOST_TEST(bus.CountEvents(two.subscriber) == 1);

    const auto claimed = bus.ClaimEvents(one.subscriber, 10, std::chrono::seconds(60));
    BOOST_TEST_REQUIRE(claimed.size() == 1U);
    BOOST_TEST(bus.AckEvent(one.subscriber, claimed[0].eventId));

    BOOST_TEST(bus.CountEvents(one.subscriber) == 0);
    BOOST_TEST(bus.CountEvents(two.subscriber) == 1);
}

BOOST_AUTO_TEST_CASE(AFilterDecidesWhatIsStoredAtAll) {
    if (!databaseAvailable()) return;
    Fixture fixture;

    auto &bus = EventBus::instance();
    bus.SubscribeExternal(fixture.subscriber, fixture.eventType,
                          boost::json::object{{"bucketName", "inbox"}}, "000000000000");

    bus.Publish(fixture.eventType, boost::json::value{{"bucketName", "inbox"}, {"key", "a.csv"}}, "test");
    bus.Publish(fixture.eventType, boost::json::value{{"bucketName", "payroll"}, {"key", "b.csv"}}, "test");

    // Evaluated when the event is published, so a subscriber's backlog is proportional to what it
    // asked for rather than to how busy the installation is.
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 1);
    const auto claimed = bus.ClaimEvents(fixture.subscriber, 10, std::chrono::seconds(60));
    BOOST_TEST_REQUIRE(claimed.size() == 1U);
    BOOST_TEST(claimed[0].payload.at("key").as_string() == "a.csv");
}

BOOST_AUTO_TEST_CASE(AnEventForAnotherAccountIsNeverStored) {
    if (!databaseAvailable()) return;
    Fixture fixture;

    auto &bus = EventBus::instance();
    bus.SubscribeExternal(fixture.subscriber, fixture.eventType, {}, "000000000000");

    bus.Publish(fixture.eventType, boost::json::value{{"accountId", "999999999999"}}, "test");
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 0);

    bus.Publish(fixture.eventType, boost::json::value{{"accountId", "000000000000"}}, "test");
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 1);
}

BOOST_AUTO_TEST_CASE(UnsubscribingTakesTheBacklogWithIt) {
    if (!databaseAvailable()) return;
    Fixture fixture;

    auto &bus = EventBus::instance();
    bus.SubscribeExternal(fixture.subscriber, fixture.eventType, {}, "000000000000");
    bus.Publish(fixture.eventType, boost::json::value{{"n", 1}}, "test");
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 1);

    BOOST_TEST(bus.UnsubscribeExternal(fixture.subscriber, "") == 1);
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 0);
    BOOST_TEST(bus.ListSubscriptions(fixture.subscriber).empty());

    // And nothing is stored for it afterwards.
    bus.Publish(fixture.eventType, boost::json::value{{"n", 2}}, "test");
    BOOST_TEST(bus.CountEvents(fixture.subscriber) == 0);
}
