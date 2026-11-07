// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmListMatchTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <set>
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
#include <euclid/database/RepositoryFactory.h>

// list-buckets and list-objects with a "matchType": the page and the total have to agree on what
// was selected, every type but "regex" has to take the text literally, and a pattern that does
// not compile is the caller's 400, not the database's 500. Driven through ESM's socket against
// the in-memory store.

namespace {

    namespace http = boost::beast::http;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-list-match-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    std::string bucketErn(const std::string &name) { return Euclid::Core::createEsmBucketErn(kAccount, kNamespace, name); }

    Euclid::ESM::EsmServer &server() {
        static Euclid::ESM::EsmServer *instance = [] {
            std::filesystem::create_directories(scratch() / "esm");
            auto *created = new Euclid::ESM::EsmServer((scratch() / "esm.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    void addBucket(const std::string &name) {
        Euclid::Database::Entity::ESM::Bucket bucket;
        bucket.name = name;
        bucket.ern = bucketErn(name);
        bucket.accountId = kAccount;
        bucket.nameSpace = kNamespace;
        bucket.region = kRegion;
        std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->upsertBucket(bucket);
    }

    struct Answer {
        int status = 0;
        std::string body;

        [[nodiscard]] boost::json::object json() const {
            boost::system::error_code ec;
            const auto parsed = boost::json::parse(body, ec);
            return !ec && parsed.is_object() ? parsed.as_object() : boost::json::object{};
        }
    };

    Answer send(const std::string &action, const std::string &body, const std::map<std::string, std::string> &headers = {}) {
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
        return {static_cast<int>(res.result_int()), res.body()};
    }

    Answer ask(const std::string &action, const boost::json::object &body) {
        return send(action, boost::json::serialize(body));
    }

    struct Fixture {
        Fixture() {
            auto &configuration = Euclid::Core::Configuration::instance();
            configuration.set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            configuration.set<std::string>("euclid.modules.esm.data-dir", (scratch() / "esm").string());
            Euclid::Database::Database::instance().initializeMemory();
            std::ignore = server();

            Euclid::Database::Entity::EAM::User admin;
            admin.userId = "admin";
            admin.email = "admin@example.org";
            admin.accountId = kAccount;
            admin.region = kRegion;
            admin.ern = std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:admin";
            std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->upsertUser(admin);

            // "billing.in" and "billingXin" are what a "." read as a pattern confuses.
            for (const auto *name: {"orders-in", "orders-out", "my-orders", "billing.in", "billingXin"}) addBucket(name);
        }
    };

    // The names on the page, and the total that came with it.
    std::pair<std::set<std::string>, long> listBuckets(const boost::json::object &body) {
        const auto answer = ask("list-buckets", body);
        BOOST_REQUIRE(answer.status == 200);
        const auto json = answer.json();
        std::set<std::string> names;
        for (const auto &bucket: json.at("buckets").as_array()) names.insert(std::string(bucket.at("name").as_string()));
        return {names, json.at("total").as_int64()};
    }

    std::pair<std::set<std::string>, long> listObjects(const std::string &prefix, const std::string &matchType) {
        const auto answer = ask("list-objects", {{"bucketErn", bucketErn("orders-in")}, {"prefix", prefix}, {"matchType", matchType}, {"pageSize", 10}});
        BOOST_REQUIRE(answer.status == 200);
        const auto json = answer.json();
        std::set<std::string> keys;
        for (const auto &object: json.at("objects").as_array()) keys.insert(std::string(object.at("key").as_string()));
        return {keys, json.at("total").as_int64()};
    }

    using Names = std::set<std::string>;

}// namespace

BOOST_FIXTURE_TEST_CASE(NoMatchTypeIsALiteralPrefix, Fixture) {
    const auto [names, total] = listBuckets({{"prefix", "billing."}, {"pageSize", 10}});
    BOOST_TEST((names == Names{"billing.in"}));
    BOOST_TEST(total == 1);
}

BOOST_FIXTURE_TEST_CASE(ContainsFindsTheTextAnywhere, Fixture) {
    const auto [names, total] = listBuckets({{"prefix", "orders"}, {"matchType", "contains"}, {"pageSize", 10}});
    BOOST_TEST((names == Names{"orders-in", "orders-out", "my-orders"}));
    BOOST_TEST(total == 3);
}

BOOST_FIXTURE_TEST_CASE(TheTotalCountsTheMatchNotThePage, Fixture) {
    const auto [names, total] = listBuckets({{"prefix", "orders"}, {"matchType", "contains"}, {"pageSize", 2}, {"pageIndex", 0}});
    BOOST_TEST(names.size() == 2u);
    BOOST_TEST(total == 3);
}

BOOST_FIXTURE_TEST_CASE(EndsWithIsLiteral, Fixture) {
    const auto [names, total] = listBuckets({{"prefix", ".in"}, {"matchType", "endsWith"}, {"pageSize", 10}});
    BOOST_TEST((names == Names{"billing.in"}));
    BOOST_TEST(total == 1);
}

BOOST_FIXTURE_TEST_CASE(RegexIsReadAsAPattern, Fixture) {
    const auto [names, total] = listBuckets({{"prefix", "^billing.in$"}, {"matchType", "regex"}, {"pageSize", 10}});
    BOOST_TEST((names == Names{"billing.in", "billingXin"}));
    BOOST_TEST(total == 2);
}

BOOST_FIXTURE_TEST_CASE(AnInvalidRegexIsABadRequest, Fixture) {
    const auto answer = ask("list-buckets", {{"prefix", "orders(["}, {"matchType", "regex"}});
    BOOST_TEST(answer.status == 400);
    BOOST_TEST(std::string(answer.json().at("error").as_string()).starts_with("Invalid regular expression: "));
}

BOOST_FIXTURE_TEST_CASE(AnUnknownMatchTypeIsABadRequest, Fixture) {
    const auto answer = ask("list-buckets", {{"prefix", "orders"}, {"matchType", "glob"}});
    BOOST_TEST(answer.status == 400);
    BOOST_TEST(std::string(answer.json().at("error").as_string()) == "matchType must be prefix, endsWith, contains or regex");
}

BOOST_FIXTURE_TEST_CASE(ListObjectsHonoursTheMatchType, Fixture) {
    for (const auto *key: {"docs/report.xml", "docs/reportXxml", "img/logo.png", "img/report.png"}) {
        BOOST_REQUIRE(send("put-object", "data", {{"x-euclid-bucket-ern", bucketErn("orders-in")}, {"x-euclid-key", key}}).status == 200);
    }

    {
        const auto [keys, total] = listObjects("report", "contains");
        BOOST_TEST((keys == Names{"docs/report.xml", "docs/reportXxml", "img/report.png"}));
        BOOST_TEST(total == 3);
    }
    {
        const auto [keys, total] = listObjects(".xml", "endsWith");
        BOOST_TEST((keys == Names{"docs/report.xml"}));
        BOOST_TEST(total == 1);
    }
    {
        const auto [keys, total] = listObjects(R"(^img/.*\.png$)", "regex");
        BOOST_TEST((keys == Names{"img/logo.png", "img/report.png"}));
        BOOST_TEST(total == 2);
    }

    const auto invalid = ask("list-objects", {{"bucketErn", bucketErn("orders-in")}, {"prefix", "*"}, {"matchType", "regex"}});
    BOOST_TEST(invalid.status == 400);
}

BOOST_FIXTURE_TEST_CASE(ALevelListingTakesOnlyAPrefix, Fixture) {
    const auto answer = ask("list-objects", {{"bucketErn", bucketErn("orders-in")}, {"prefix", "docs/"}, {"delimiter", "/"}, {"matchType", "contains"}});
    BOOST_TEST(answer.status == 400);
}
