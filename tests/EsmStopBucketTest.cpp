// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmStopBucketTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <string>
#include <unistd.h>

// Boost includes
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <EsmServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/RepositoryFactory.h>

// "esm stop-bucket": objects are written as before, their events are made and stored, and nobody
// can receive one until "esm start-bucket" - then every one of them, in order. Driven through ESM's
// socket as an administrator, with an EES subscriber to esm.object.created reading what arrives.

namespace {

    namespace http = boost::beast::http;
    using Euclid::Database::EventBus;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";
    constexpr auto kSubscriber = "invoice-service";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-stop-bucket-test-" + std::to_string(::getpid()));
        return path;
    }

    std::string bucketErn() { return Euclid::Core::createEsmBucketErn(kAccount, kNamespace, "incoming"); }

    Euclid::ESM::EsmServer &server() {
        static Euclid::ESM::EsmServer *instance = [] {
            std::filesystem::create_directories(scratch() / "esm");
            auto *created = new Euclid::ESM::EsmServer((scratch() / "esm.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    struct Fixture {
        Fixture() {
            auto &configuration = Euclid::Core::Configuration::instance();
            configuration.set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            configuration.set<std::string>("euclid.modules.esm.data-dir", (scratch() / "esm").string());
            Euclid::Database::Database::instance().initializeMemory();
            std::ignore = server();

            const auto &factory = Euclid::Database::RepositoryFactory::instance();
            Euclid::Database::Entity::EAM::User admin;
            admin.userId = "admin";
            admin.email = "admin@example.org";
            admin.accountId = kAccount;
            admin.region = kRegion;
            admin.ern = std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:admin";
            std::ignore = factory.eamRepository()->upsertUser(admin);

            Euclid::Database::Entity::ESM::Bucket bucket;
            bucket.name = "incoming";
            bucket.ern = bucketErn();
            bucket.accountId = kAccount;
            bucket.nameSpace = kNamespace;
            bucket.region = kRegion;
            std::ignore = factory.esmRepository()->upsertBucket(bucket);

            EventBus::instance().SubscribeExternal(kSubscriber, "esm.object.created", {}, kAccount);
        }
    };

    int call(const std::string &action, const std::string &body, const std::map<std::string, std::string> &headers = {}) {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken("admin", kJwtSecret));
        req.set("x-euclid-target", "esm");
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", kNamespace);
        for (const auto &[name, value]: headers) req.set(name, value);
        req.body() = body;
        req.prepare_payload();

        boost::asio::io_context ioc;
        boost::asio::local::stream_protocol::socket socket(ioc);
        socket.connect(boost::asio::local::stream_protocol::endpoint((scratch() / "esm.sock").string()));
        http::write(socket, req);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        return static_cast<int>(res.result_int());
    }

    int put(const std::string &key) {
        return call("put-object", "hello", {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", key}});
    }

    int setStopped(const bool stop) {
        return call(stop ? "stop-bucket" : "start-bucket", boost::json::serialize(boost::json::object{{"ern", bucketErn()}}));
    }

    std::vector<Euclid::Database::EventEnvelope> receive() {
        return EventBus::instance().ClaimEvents(kSubscriber, 10, std::chrono::seconds(30));
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EsmStopBucketTest, Fixture)

    BOOST_AUTO_TEST_CASE(AnObjectInARunningBucketIsAnnounced) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        BOOST_TEST(receive().size() == 1U);
    }

    // Written as before - a stopped bucket is not a closed one - and its event made and kept.
    BOOST_AUTO_TEST_CASE(AStoppedBucketsEventsAreKeptAndNotReceived) {
        BOOST_TEST_REQUIRE(setStopped(true) == 200);
        BOOST_TEST(put("a.txt") == 200);
        BOOST_TEST(receive().empty());
        BOOST_TEST(EventBus::instance().CountHeld(bucketErn()) == 1L);
        BOOST_TEST(Euclid::Database::RepositoryFactory::instance().esmRepository()->findBucketByErn(bucketErn())->stopped);
    }

    BOOST_AUTO_TEST_CASE(StartingTheBucketDeliversWhatWasHeldInOrder) {
        BOOST_TEST_REQUIRE(setStopped(true) == 200);
        BOOST_TEST_REQUIRE(put("first.txt") == 200);
        BOOST_TEST_REQUIRE(put("second.txt") == 200);

        BOOST_TEST_REQUIRE(setStopped(false) == 200);
        const auto events = receive();
        BOOST_TEST_REQUIRE(events.size() == 2U);
        BOOST_TEST(std::string(events[0].payload.at("key").as_string()) == "first.txt");
        BOOST_TEST(std::string(events[1].payload.at("key").as_string()) == "second.txt");

        // And from then on, as though it had never been stopped.
        BOOST_TEST_REQUIRE(put("third.txt") == 200);
        BOOST_TEST(receive().size() == 1U);
    }

    // Twice is once: a second stop holds nothing new, and starting a running bucket releases nothing.
    BOOST_AUTO_TEST_CASE(StoppingAndStartingAreIdempotent) {
        BOOST_TEST(setStopped(true) == 200);
        BOOST_TEST(setStopped(true) == 200);
        BOOST_TEST(setStopped(false) == 200);
        BOOST_TEST(setStopped(false) == 200);
        BOOST_TEST(!Euclid::Database::RepositoryFactory::instance().esmRepository()->findBucketByErn(bucketErn())->stopped);
    }

BOOST_AUTO_TEST_SUITE_END()
