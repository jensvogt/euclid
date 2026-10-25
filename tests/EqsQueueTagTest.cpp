// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EqsQueueTagTest
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
#include <EqsServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/eap/InfrastructureApply.h>

// What an application may tag. It holds eqs:add-queue-tag through the built-in application role so
// that euclid-spring can stamp the heartbeat on its own delivery queue - which every one of its
// 26,000 attempts was refused before the permission was added. Holding it must not let it tag any
// queue in the account, and must never let it write the owner tag `eap apply` decides by.
//
// Driven through the server's socket with the real authorization lookup wired, so the role gate and
// the resource check are the ones a module runs.

namespace {

    namespace http = boost::beast::http;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-queue-tag-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    std::string queueErn(const std::string &name) { return Euclid::Core::createEqsQueueErn(kAccount, kNamespace, name); }
    std::string userErn(const std::string &userId) { return std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:" + userId; }

    Euclid::EQS::EqsServer &server() {
        static Euclid::EQS::EqsServer *instance = [] {
            std::filesystem::create_directories(scratch());
            auto *created = new Euclid::EQS::EqsServer((scratch() / "eqs.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    void user(const std::string &userId) {
        Euclid::Database::Entity::EAM::User u;
        u.userId = userId;
        u.email = userId + "@example.org";
        u.accountId = kAccount;
        u.region = kRegion;
        u.ern = userErn(userId);
        std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->upsertUser(u);
    }

    void queue(const std::string &name, const std::string &owner = "eap", const bool internal = false) {
        Euclid::Database::Entity::EQS::Queue q;
        q.name = name;
        q.ern = queueErn(name);
        q.accountId = kAccount;
        q.nameSpace = kNamespace;
        q.region = kRegion;
        q.owner = owner;
        q.internal = internal;
        std::ignore = Euclid::Database::RepositoryFactory::instance().eqsRepository()->upsertQueue(q);
    }

    struct Fixture {
        Fixture() {
            Euclid::Core::Configuration::instance().set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            Euclid::Database::Database::instance().initializeMemory();
            std::ignore = server();
            Euclid::Database::WireAuthorizationLookup();

            user("admin");
            Euclid::Database::Entity::EAM::UserGroup administrators;
            administrators.name = Euclid::Database::kEamAdministratorGroupName;
            administrators.accountId = kAccount;
            administrators.region = kRegion;
            administrators.userIds = {"admin"};
            std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->upsertUserGroup(administrators);

            // An application principal, granted the application role on one queue - what EAP grants
            // once a deployment names its resources.
            user("app-orders");
            Euclid::Database::Entity::EAM::Grant grant;
            grant.role = "application";
            grant.principal = userErn("app-orders");
            grant.accountId = kAccount;
            grant.namespaces = {kNamespace};
            grant.resources = {queueErn("orders-in")};
            std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->addGrant(grant);

            queue("orders-in");
            queue("billing-in");
            // A listener's delivery queue: internal, and owned by the principal that made it.
            queue("orders-orders-in-onMessage-run1", "app-orders", true);
        }
    };

    int tag(const std::string &as, const std::string &queueName, const std::string &key, const std::string &action = "add-queue-tag") {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken(as, kJwtSecret));
        req.set("x-euclid-target", "eqs");
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", kNamespace);
        req.set("x-euclid-user-id", as);
        req.body() = boost::json::serialize(boost::json::object{{"ern", queueErn(queueName)}, {"key", key}, {"value", "x"}});
        req.prepare_payload();

        boost::asio::io_context ioc;
        boost::asio::local::stream_protocol::socket socket(ioc);
        socket.connect(boost::asio::local::stream_protocol::endpoint((scratch() / "eqs.sock").string()));
        http::write(socket, req);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        return static_cast<int>(res.result_int());
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EqsQueueTagTest, Fixture)

    // The heartbeat, which is the reason the application role holds the permission at all.
    BOOST_AUTO_TEST_CASE(AnApplicationStampsTheHeartbeatOnItsOwnDeliveryQueue) {
        BOOST_TEST(tag("app-orders", "orders-orders-in-onMessage-run1", "euclid.listener.heartbeat") == 200);
    }

    BOOST_AUTO_TEST_CASE(AnApplicationTagsAQueueItIsGranted) {
        BOOST_TEST(tag("app-orders", "orders-in", "team") == 200);
    }

    // Before, add-queue-tag checked nothing about the queue: whoever held the permission could tag
    // every queue in the account.
    BOOST_AUTO_TEST_CASE(AnApplicationCannotTagAQueueItIsNotGranted) {
        BOOST_TEST(tag("app-orders", "billing-in", "team") == 403);
    }

    // The tag `eap apply` adopts, refuses and removes by. Written by an application it would let one
    // claim a queue, or hand it to another application to delete.
    BOOST_AUTO_TEST_CASE(OnlyAnAdministratorWritesTheOwnerTag) {
        const std::string owner = Euclid::Database::Entity::EAP::Infrastructure::kOwnerTag;
        BOOST_TEST(tag("app-orders", "orders-in", owner) == 403);
        BOOST_TEST(tag("app-orders", "orders-orders-in-onMessage-run1", owner) == 403);
        BOOST_TEST(tag("admin", "billing-in", owner) == 200);
    }

    // Removing it is as much a takeover as writing it: an untagged queue is adopted by the next apply
    // that declares it.
    BOOST_AUTO_TEST_CASE(OnlyAnAdministratorRemovesTheOwnerTag) {
        const std::string owner = Euclid::Database::Entity::EAP::Infrastructure::kOwnerTag;
        BOOST_TEST(tag("admin", "orders-in", owner) == 200);
        BOOST_TEST(tag("app-orders", "orders-in", owner, "delete-queue-tag") == 403);
    }

BOOST_AUTO_TEST_SUITE_END()
