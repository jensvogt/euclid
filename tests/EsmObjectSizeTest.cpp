// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmObjectSizeTest
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

// "esm get-object-size": an object's size in bytes, by its ERN or by bucket and key, without
// fetching it. And get-bucket-size beside it, which dereferenced an empty optional for a bucket
// that did not exist. Driven through ESM's socket as an administrator.

namespace {

    namespace http = boost::beast::http;
    using Euclid::Database::EventBus;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-object-size-test-" + std::to_string(std::random_device{}()));
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

BOOST_FIXTURE_TEST_SUITE(EsmObjectSizeTest, Fixture)

    BOOST_AUTO_TEST_CASE(AnObjectsSizeIsItsBytes) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        const auto [status, body] = ask("get-object-size", {{"bucketErn", bucketErn()}, {"key", "a.txt"}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("size").as_int64() == 12);
        BOOST_TEST(std::string(body.at("key").as_string()) == "a.txt");
    }

    // The bucket by name, as every other bucketErn may be given.
    BOOST_AUTO_TEST_CASE(TheBucketCanBeNamed) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        BOOST_TEST(ask("get-object-size", {{"bucketErn", "incoming"}, {"key", "a.txt"}}).first == 200);
    }

    BOOST_AUTO_TEST_CASE(AnObjectCanBeNamedByItsErn) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        const auto ern = std::string(ask("get-object-size", {{"bucketErn", bucketErn()}, {"key", "a.txt"}}).second.at("ern").as_string());
        const auto [status, body] = ask("get-object-size", {{"ern", ern}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("size").as_int64() == 12);
    }

    BOOST_AUTO_TEST_CASE(AnObjectThatIsNotThereIsNotFound) {
        BOOST_TEST(ask("get-object-size", {{"bucketErn", bucketErn()}, {"key", "missing.txt"}}).first == 404);
    }

    BOOST_AUTO_TEST_CASE(TheObjectHasToBeNamedOneWay) {
        BOOST_TEST(ask("get-object-size", {{"bucketErn", bucketErn()}}).first == 400);
        BOOST_TEST(ask("get-object-size", {}).first == 400);
    }

    // It read bucket->size before checking there was a bucket.
    BOOST_AUTO_TEST_CASE(TheSizeOfABucketThatIsNotThereIsNotFound) {
        BOOST_TEST(ask("get-bucket-size", {{"ern", "ern:esm:eu-central-1:000000000000:development:bucket:nowhere"}}).first == 404);
    }

BOOST_AUTO_TEST_SUITE_END()
