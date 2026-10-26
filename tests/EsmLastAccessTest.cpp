// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmLastAccessTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <random>
#include <string>

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

// Object::lastAccessed, the first piece of ESM lifecycle management: which actions record that an
// object was used, which do not, and that a read records it at most once per granularity. Driven
// through ESM's socket as an administrator.

namespace {

    namespace http = boost::beast::http;
    using Euclid::Database::EventBus;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-last-access-test-" + std::to_string(std::random_device{}()));
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

    std::pair<int, boost::json::object> ask(const std::string &action, const boost::json::object &body) {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken("admin", kJwtSecret));
        req.set("x-euclid-target", "esm");
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", kNamespace);
        req.body() = boost::json::serialize(body);
        req.prepare_payload();

        boost::asio::io_context ioc;
        boost::asio::local::stream_protocol::socket socket(ioc);
        socket.connect(boost::asio::local::stream_protocol::endpoint((scratch() / "esm.sock").string()));
        http::write(socket, req);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        const auto parsed = boost::json::parse(res.body());
        return {static_cast<int>(res.result_int()), parsed.is_object() ? parsed.as_object() : boost::json::object{}};
    }

    int put(const std::string &key) {
        return call("put-object", "hello, world", {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", key}});
    }



}// namespace

namespace {

    using Euclid::Database::Entity::ESM::Object;

    Object stored(const std::string &key) {
        const auto object = Euclid::Database::RepositoryFactory::instance().esmRepository()->findObjectByBucketAndKey(bucketErn(), key);
        BOOST_TEST_REQUIRE(object.has_value());
        return *object;
    }

    int getObject(const std::string &key) {
        return call("get-object", "", {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", key}, {"x-euclid-part-size", "1048576"}});
    }

    // Moved into the past, so a later access is visibly later and the granularity has passed.
    void backdate(const std::string &key, const std::chrono::hours by) {
        const auto object = stored(key);
        std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->recordObjectAccess(
                object.ern, std::chrono::system_clock::now() - by);
    }

    void granularity(const long seconds) {
        Euclid::Core::Configuration::instance().set<long>("euclid.modules.esm.last-access-granularity", seconds);
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EsmLastAccessTest, Fixture)

    BOOST_AUTO_TEST_CASE(WritingAnObjectIsAnAccess) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        BOOST_TEST(stored("a.txt").lastAccessed.has_value());
    }

    BOOST_AUTO_TEST_CASE(ReadingAnObjectIsAnAccessOnceTheGranularityHasPassed) {
        granularity(3600);
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        backdate("a.txt", std::chrono::hours(2));
        const auto before = *stored("a.txt").lastAccessed;

        BOOST_TEST_REQUIRE(getObject("a.txt") == 200);
        BOOST_TEST((*stored("a.txt").lastAccessed > before));
    }

    // The relatime rule: a read within the granularity of the last one is not written down, so the
    // busiest path ESM has does not become a write per read.
    BOOST_AUTO_TEST_CASE(ARecentAccessIsNotWrittenAgain) {
        granularity(3600);
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        backdate("a.txt", std::chrono::hours(0));
        const auto before = *stored("a.txt").lastAccessed;

        BOOST_TEST_REQUIRE(getObject("a.txt") == 200);
        BOOST_TEST((*stored("a.txt").lastAccessed == before));
    }

    BOOST_AUTO_TEST_CASE(StartingADownloadIsAnAccess) {
        granularity(0);
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        backdate("a.txt", std::chrono::hours(2));
        const auto before = *stored("a.txt").lastAccessed;

        BOOST_TEST(ask("create-download", {{"bucketErn", bucketErn()}, {"key", "a.txt"}}).first == 200);
        BOOST_TEST((*stored("a.txt").lastAccessed > before));
    }

    // Asking about an object is not using it.
    BOOST_AUTO_TEST_CASE(AskingTheSizeIsNotAnAccess) {
        granularity(0);
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        backdate("a.txt", std::chrono::hours(2));
        const auto before = *stored("a.txt").lastAccessed;

        BOOST_TEST_REQUIRE(ask("get-object-size", {{"bucketErn", bucketErn()}, {"key", "a.txt"}}).first == 200);
        BOOST_TEST_REQUIRE(ask("list-objects", {{"bucketErn", bucketErn()}}).first == 200);
        BOOST_TEST((*stored("a.txt").lastAccessed == before));
    }

    BOOST_AUTO_TEST_CASE(ACopyAccessesTheSourceAndTheCopy) {
        granularity(0);
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        backdate("a.txt", std::chrono::hours(2));
        const auto before = *stored("a.txt").lastAccessed;

        BOOST_TEST_REQUIRE(ask("copy-object", {{"sourceBucketErn", bucketErn()}, {"sourceKey", "a.txt"},
                                               {"targetBucketErn", bucketErn()}, {"targetKey", "b.txt"}}).first == 200);
        BOOST_TEST((*stored("a.txt").lastAccessed > before));
        BOOST_TEST((*stored("b.txt").lastAccessed > before));
    }

    BOOST_AUTO_TEST_CASE(AMovedObjectIsAccessed) {
        granularity(0);
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        backdate("a.txt", std::chrono::hours(2));
        const auto before = *stored("a.txt").lastAccessed;

        BOOST_TEST_REQUIRE(ask("move-object", {{"sourceBucketErn", bucketErn()}, {"sourceKey", "a.txt"},
                                               {"targetBucketErn", bucketErn()}, {"targetKey", "b.txt"}}).first == 200);
        BOOST_TEST((*stored("b.txt").lastAccessed > before));
    }

    // Rewritten for another reason, an object keeps the access it had rather than losing it.
    BOOST_AUTO_TEST_CASE(SettingAnAttributeLeavesTheAccessAlone) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        backdate("a.txt", std::chrono::hours(5));
        const auto before = *stored("a.txt").lastAccessed;

        BOOST_TEST_REQUIRE(ask("add-object-attribute", {{"ern", stored("a.txt").ern}, {"name", "team"},
                                                        {"value", {{"type", "string"}, {"value", "pim"}}}}).first == 200);
        BOOST_TEST_REQUIRE(stored("a.txt").lastAccessed.has_value());
        BOOST_TEST((*stored("a.txt").lastAccessed == before));
    }

    // An object stored before access was recorded is last used when it was last modified - not
    // never, which would expire every old object the first time a lifecycle rule ran.
    BOOST_AUTO_TEST_CASE(AnObjectWithNoAccessWasLastUsedWhenModified) {
        Object object;
        object.modified = std::chrono::system_clock::now() - std::chrono::hours(48);
        BOOST_TEST((Euclid::Database::Entity::ESM::LastUsed(object) == object.modified));
        BOOST_TEST(Euclid::Database::Entity::ESM::AccessWorthRecording(object, std::chrono::system_clock::now(), std::chrono::hours(1)));
    }

    // And the listing says when it was last used.
    BOOST_AUTO_TEST_CASE(AListingShowsTheLastAccess) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        const auto [status, body] = ask("list-objects", {{"bucketErn", bucketErn()}});
        BOOST_TEST_REQUIRE(status == 200);
        const auto &objects = body.at("objects").as_array();
        BOOST_TEST_REQUIRE(objects.size() == 1U);
        BOOST_TEST(!objects[0].as_object().at("lastAccessed").as_string().empty());
    }

BOOST_AUTO_TEST_SUITE_END()
