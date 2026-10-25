// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EkvTableAccessTest
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
#include <EkvServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>

// A grant on one table is a grant on that table. No table action was resource-checked before, so a
// principal holding ekv:get-item could read every table in its namespace and a declaration naming one
// table granted all of them. Driven through the socket with the real authorization lookup wired.

namespace {

    namespace http = boost::beast::http;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-ekv-access-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    std::string tableErn(const std::string &name) { return Euclid::Core::createEkvTableErn(kAccount, kNamespace, name); }
    std::string userErn(const std::string &userId) { return std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:" + userId; }

    Euclid::EKV::EkvServer &server() {
        static Euclid::EKV::EkvServer *instance = [] {
            std::filesystem::create_directories(scratch());
            auto *created = new Euclid::EKV::EkvServer((scratch() / "ekv.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    void table(const std::string &name) {
        Euclid::Database::Entity::EKV::Table t;
        t.name = name;
        t.accountId = kAccount;
        t.nameSpace = kNamespace;
        t.region = kRegion;
        t.ern = tableErn(name);
        t.partitionKey = {.name = "id", .type = Euclid::Database::Entity::EKV::KeyType::String};
        std::ignore = Euclid::Database::RepositoryFactory::instance().ekvRepository()->createTable(t);
    }

    void grant(const std::string &principal, const std::string &role, const std::vector<std::string> &resources) {
        Euclid::Database::Entity::EAM::User u;
        u.userId = principal;
        u.email = principal + "@example.org";
        u.accountId = kAccount;
        u.region = kRegion;
        u.ern = userErn(principal);
        std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->upsertUser(u);

        Euclid::Database::Entity::EAM::Grant g;
        g.role = role;
        g.principal = userErn(principal);
        g.accountId = kAccount;
        g.namespaces = {kNamespace};
        g.resources = resources;
        std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->addGrant(g);
    }

    struct Fixture {
        Fixture() {
            Euclid::Core::Configuration::instance().set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            Euclid::Database::Database::instance().initializeMemory();
            std::ignore = server();
            Euclid::Database::WireAuthorizationLookup();

            // The role EAP writes for a table's read level.
            Euclid::Database::Entity::EAM::Role reader;
            reader.name = "access-table-read";
            reader.accountId = kAccount;
            reader.permissions = {"ekv:get-table", "ekv:get-item", "ekv:query", "ekv:scan"};
            std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->upsertRole(reader);

            table("orders-state");
            table("billing-state");
            grant("app-orders", "access-table-read", {tableErn("orders-state")});
            // What every grant an installation had looked like when the check was added.
            grant("app-legacy", "access-table-read", {"*"});
        }
    };

    int getItem(const std::string &as, const std::string &tableName) {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken(as, kJwtSecret));
        req.set("x-euclid-target", "ekv");
        req.set("x-euclid-action", "get-item");
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", kNamespace);
        req.set("x-euclid-user-id", as);
        req.body() = boost::json::serialize(boost::json::object{{"table", tableName}, {"key", boost::json::object{{"id", "o-1"}}}});
        req.prepare_payload();

        boost::asio::io_context ioc;
        boost::asio::local::stream_protocol::socket socket(ioc);
        socket.connect(boost::asio::local::stream_protocol::endpoint((scratch() / "ekv.sock").string()));
        http::write(socket, req);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        return static_cast<int>(res.result_int());
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EkvTableAccessTest, Fixture)

    // Granted the table, and there is no such item: an answer about the item, not a refusal.
    BOOST_AUTO_TEST_CASE(AGrantOnATableReachesThatTable) {
        BOOST_TEST(getItem("app-orders", "orders-state") == 404);
    }

    BOOST_AUTO_TEST_CASE(AGrantOnATableDoesNotReachAnother) {
        BOOST_TEST(getItem("app-orders", "billing-state") == 403);
    }

    // Refused before the lookup, so a narrow grant cannot find out which tables exist.
    BOOST_AUTO_TEST_CASE(ATableOutsideTheGrantIsRefusedWhetherOrNotItExists) {
        BOOST_TEST(getItem("app-orders", "no-such-table") == 403);
    }

    BOOST_AUTO_TEST_CASE(AGrantOnEverythingIsStillEverything) {
        BOOST_TEST(getItem("app-legacy", "billing-state") == 404);
    }

BOOST_AUTO_TEST_SUITE_END()
