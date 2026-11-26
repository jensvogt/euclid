// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EagCopyRouteTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <EagServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>

// eag:copy-route, driven through the module's socket against the in-memory store: the copy carries
// everything the source has and only what is named changes, the source is left alone, and the copy
// is held to the rules an update is - its path and methods may not collide, its application has to
// exist in its namespace, and it cannot be the source again under the same name. update-route,
// whose field handling copy-route now shares, is checked to still work.

namespace {

    namespace http = boost::beast::http;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kDevelopment = "development";
    constexpr auto kIntegration = "integration";
    constexpr auto kAdmin = "route-admin";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-eag-copy-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    unsigned short freePort() {
        boost::asio::io_context context;
        boost::asio::ip::tcp::acceptor probe(context, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
        return probe.local_endpoint().port();
    }

    Euclid::EAG::EagServer &server() {
        static Euclid::EAG::EagServer *instance = [] {
            std::filesystem::create_directories(scratch());
            // The proxy port is not what is tested; a free one keeps it from colliding with a real
            // gateway on this host.
            Euclid::Core::Configuration::instance().set<long>("euclid.modules.eag.port", freePort());
            auto *created = new Euclid::EAG::EagServer((scratch() / "eag.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    void application(const std::string &applicationId, const std::string &nameSpace) {
        Euclid::Database::Entity::EAP::Application app;
        app.applicationId = applicationId;
        app.runtimeName = applicationId + "-" + nameSpace;
        app.accountId = kAccount;
        app.region = kRegion;
        app.nameSpace = nameSpace;
        app.ern = "ern:eap:" + std::string(kRegion) + ":" + kAccount + ":" + nameSpace + ":application:" + applicationId;
        std::ignore = Euclid::Database::RepositoryFactory::instance().eapRepository()->upsertApplication(app);
    }

    struct Fixture {
        Fixture() {
            Euclid::Core::Configuration::instance().set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            Euclid::Database::Database::instance().initializeMemory();
            std::ignore = server();

            const auto eam = Euclid::Database::RepositoryFactory::instance().eamRepository();
            Euclid::Database::Entity::EAM::User user;
            user.userId = kAdmin;
            user.email = std::string(kAdmin) + "@example.org";
            user.accountId = kAccount;
            user.region = kRegion;
            user.ern = std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:" + kAdmin;
            std::ignore = eam->upsertUser(user);

            // Every route action is administrator work.
            Euclid::Database::Entity::EAM::UserGroup administrators;
            administrators.name = Euclid::Database::kEamAdministratorGroupName;
            administrators.accountId = kAccount;
            administrators.region = kRegion;
            administrators.userIds = {kAdmin};
            std::ignore = eam->upsertUserGroup(administrators);

            application("orders", kDevelopment);
            application("orders", kIntegration);
            application("billing", kDevelopment);
        }
    };

    std::pair<int, boost::json::value> call(const std::string &action, const boost::json::object &body,
                                            const std::string &nameSpace = kDevelopment) {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken(kAdmin, kJwtSecret));
        req.set("x-euclid-target", "eag");
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", nameSpace);
        req.set("x-euclid-user-id", kAdmin);
        req.body() = boost::json::serialize(body);
        req.prepare_payload();

        boost::asio::io_context ioc;
        boost::asio::local::stream_protocol::socket socket(ioc);
        socket.connect(boost::asio::local::stream_protocol::endpoint((scratch() / "eag.sock").string()));
        http::write(socket, req);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);

        boost::system::error_code ec;
        auto parsed = boost::json::parse(res.body(), ec);
        return {static_cast<int>(res.result_int()), ec ? boost::json::value{} : std::move(parsed)};
    }

    // A source route of its own per test, so the cases do not depend on the order they run in.
    std::string source(const std::string &routeId, const std::string &path) {
        const auto [status, body] = call("create-route", {{"routeId", routeId},
                                                          {"path", path},
                                                          {"applicationId", "orders"},
                                                          {"methods", boost::json::array{"GET"}},
                                                          {"authentication", "EUCLID"},
                                                          {"timeout", 120}});
        BOOST_TEST_REQUIRE(status == 200);
        return routeId;
    }

    boost::json::value get(const std::string &routeId, const std::string &nameSpace = kDevelopment) {
        const auto [status, body] = call("get-route", {{"routeId", routeId}}, nameSpace);
        BOOST_TEST_REQUIRE(status == 200);
        return body;
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EagCopyRoute, Fixture)

    BOOST_AUTO_TEST_CASE(IntoAnotherNamespaceUnderTheSameName) {
        const auto id = source("orders-api", "/api/orders");

        const auto [status, copy] = call("copy-route", {{"routeId", id}, {"namespace", kIntegration}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(copy.at("routeId").as_string() == id);
        BOOST_TEST(copy.at("namespace").as_string() == kIntegration);
        BOOST_TEST(copy.at("ern").as_string() == std::string("ern:eag:") + kRegion + ":" + kAccount + ":" + kIntegration + ":route:" + id);
        // Everything else carried over.
        BOOST_TEST(copy.at("path").as_string() == "/api/orders");
        BOOST_TEST(copy.at("applicationId").as_string() == "orders");
        BOOST_TEST(copy.at("authentication").as_string() == "EUCLID");
        BOOST_TEST(copy.at("timeout").as_int64() == 120);
        BOOST_TEST(copy.at("methods").as_array().size() == 1u);

        // Stored there, and the source still where it was.
        BOOST_TEST(get(id, kIntegration).at("namespace").as_string() == kIntegration);
        BOOST_TEST(get(id).at("namespace").as_string() == kDevelopment);
    }

    BOOST_AUTO_TEST_CASE(OnlyWhatIsNamedChanges) {
        const auto id = source("orders-v1", "/api/v1/orders");

        const auto [status, copy] = call("copy-route", {{"routeId", id}, {"newRouteId", "orders-v2"}, {"path", "/api/v2/orders"}, {"timeout", 30}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(copy.at("routeId").as_string() == "orders-v2");
        BOOST_TEST(copy.at("path").as_string() == "/api/v2/orders");
        BOOST_TEST(copy.at("timeout").as_int64() == 30);
        BOOST_TEST(copy.at("applicationId").as_string() == "orders");
        BOOST_TEST(copy.at("authentication").as_string() == "EUCLID");
        BOOST_TEST(copy.at("namespace").as_string() == kDevelopment);

        // The source is not touched.
        const auto original = get(id);
        BOOST_TEST(original.at("path").as_string() == "/api/v1/orders");
        BOOST_TEST(original.at("timeout").as_int64() == 120);
    }

    BOOST_AUTO_TEST_CASE(OtherMethodsOnTheSamePathToAnotherApplication) {
        const auto id = source("orders-read", "/api/shared");

        const auto [status, copy] = call("copy-route", {{"routeId", id}, {"newRouteId", "orders-write"},
                                                        {"methods", boost::json::array{"POST", "PUT"}}, {"applicationId", "billing"}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(copy.at("path").as_string() == "/api/shared");
        BOOST_TEST(copy.at("applicationId").as_string() == "billing");
        BOOST_TEST(copy.at("methods").as_array().size() == 2u);
    }

    BOOST_AUTO_TEST_CASE(TheSameRouteAgainIsRefused) {
        const auto id = source("orders-same", "/api/same");
        const auto [status, body] = call("copy-route", {{"routeId", id}});
        BOOST_TEST(status == 409);
    }

    BOOST_AUTO_TEST_CASE(ACopyThatCollidesWithItsSourceIsRefused) {
        const auto id = source("orders-collide", "/api/collide");
        BOOST_TEST(call("copy-route", {{"routeId", id}, {"newRouteId", "orders-collide-2"}}).first == 409);
        // And nothing was stored under the new name.
        BOOST_TEST(call("get-route", {{"routeId", "orders-collide-2"}}).first == 404);
    }

    BOOST_AUTO_TEST_CASE(TheApplicationHasToExistInTheCopysNamespace) {
        const auto [created, ignored] = call("create-route", {{"routeId", "billing-api"}, {"path", "/api/billing"}, {"applicationId", "billing"}});
        BOOST_TEST_REQUIRE(created == 200);
        // billing runs in development only.
        BOOST_TEST(call("copy-route", {{"routeId", "billing-api"}, {"namespace", kIntegration}}).first == 404);
        BOOST_TEST(call("get-route", {{"routeId", "billing-api"}}, kIntegration).first == 404);
    }

    BOOST_AUTO_TEST_CASE(ACopyCanStartOutOfService) {
        const auto id = source("orders-standby", "/api/standby");
        const auto [status, copy] = call("copy-route", {{"routeId", id}, {"newRouteId", "orders-standby-2"},
                                                        {"path", "/api/standby2"}, {"active", false}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(!copy.at("active").as_bool());
        BOOST_TEST(get(id).at("active").as_bool());
    }

    BOOST_AUTO_TEST_CASE(AMissingSourceIsNotFound) {
        BOOST_TEST(call("copy-route", {{"routeId", "no-such-route"}, {"newRouteId", "x"}}).first == 404);
    }

    BOOST_AUTO_TEST_CASE(AnInvalidChangeIsRefusedAndNothingIsStored) {
        const auto id = source("orders-bad", "/api/bad");
        BOOST_TEST(call("copy-route", {{"routeId", id}, {"newRouteId", "orders-bad-2"}, {"path", "no-slash"}}).first == 400);
        BOOST_TEST(call("copy-route", {{"routeId", id}, {"newRouteId", "orders-bad-2"}, {"methods", boost::json::array{"FETCH"}}}).first == 400);
        BOOST_TEST(call("get-route", {{"routeId", "orders-bad-2"}}).first == 404);
    }

    BOOST_AUTO_TEST_CASE(UpdateRouteStillChangesOnlyWhatIsNamed) {
        const auto id = source("orders-update", "/api/update");
        const auto [status, updated] = call("update-route", {{"routeId", id}, {"path", "/api/updated"}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(updated.at("path").as_string() == "/api/updated");
        BOOST_TEST(updated.at("timeout").as_int64() == 120);
        BOOST_TEST(updated.at("authentication").as_string() == "EUCLID");
        BOOST_TEST(call("update-route", {{"routeId", id}, {"applicationId", "no-such-app"}}).first == 404);
    }

BOOST_AUTO_TEST_SUITE_END()
