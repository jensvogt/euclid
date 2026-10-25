// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EapApplyInfrastructureTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

// Boost includes
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <EapServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/eap/Infrastructure.h>
#include <euclid/database/entity/eap/InfrastructureApply.h>

// `eap apply` and every deploy run the one engine in EAP. These drive it the way the CLI does -
// through apply-infrastructure, as an administrator - against the in-memory store, and look at what
// it leaves behind: the objects, their settings, their owner tag, and the application's grants.

namespace {

    namespace http = boost::beast::http;
    namespace Infra = Euclid::Database::Entity::EAP::Infrastructure;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";
    constexpr auto kApplication = "orders";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-apply-test-" + std::to_string(::getpid()));
        return path;
    }

    std::string bucketErn() { return Euclid::Core::createEsmBucketErn(kAccount, kNamespace, "apps"); }
    std::string queueErn(const std::string &name) { return Euclid::Core::createEqsQueueErn(kAccount, kNamespace, name); }
    std::string principalErn() { return std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:app-orders"; }

    // The server binds a socket on construction and serves it once started, so there is one for the
    // whole run, reached the way euclid's gateway reaches it.
    Euclid::EAP::EapServer &server() {
        static Euclid::EAP::EapServer *instance = [] {
            std::filesystem::create_directories(scratch());
            auto *created = new Euclid::EAP::EapServer((scratch() / "eap.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    http::response<http::string_body> send(const http::request<http::string_body> &req) {
        boost::asio::io_context ioc;
        boost::asio::local::stream_protocol::socket socket(ioc);
        socket.connect(boost::asio::local::stream_protocol::endpoint((scratch() / "eap.sock").string()));
        http::write(socket, req);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        return res;
    }

    struct Fixture {
        Fixture() {
            std::filesystem::create_directories(scratch() / "esm");
            Euclid::Core::Configuration::instance().set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            Euclid::Core::Configuration::instance().set<std::string>("euclid.modules.esm.data-dir", (scratch() / "esm").string());
            std::ignore = server();
            Euclid::Database::Database::instance().initializeMemory();

            const auto &factory = Euclid::Database::RepositoryFactory::instance();

            Euclid::Database::Entity::EAM::User admin;
            admin.userId = "admin";
            admin.email = "admin@example.org";
            admin.accountId = kAccount;
            admin.region = kRegion;
            admin.ern = std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:admin";
            std::ignore = factory.eamRepository()->upsertUser(admin);

            Euclid::Database::Entity::EAM::UserGroup administrators;
            administrators.name = Euclid::Database::kEamAdministratorGroupName;
            administrators.accountId = kAccount;
            administrators.region = kRegion;
            administrators.userIds = {"admin"};
            std::ignore = factory.eamRepository()->upsertUserGroup(administrators);

            Euclid::Database::Entity::EAM::User principal;
            principal.userId = "app-orders";
            principal.email = "app-orders@example.org";
            principal.accountId = kAccount;
            principal.region = kRegion;
            principal.ern = principalErn();
            std::ignore = factory.eamRepository()->upsertUser(principal);

            Euclid::Database::Entity::EAP::Application application;
            application.applicationId = kApplication;
            application.accountId = kAccount;
            application.region = kRegion;
            application.nameSpace = kNamespace;
            application.userId = "app-orders";
            application.bucketErn = bucketErn();
            std::ignore = factory.eapRepository()->upsertApplication(application);
        }
        ~Fixture() { std::filesystem::remove_all(scratch() / "esm"); }
    };

    // Stores a declaration beside the artifact, as `eap apply` does before it asks for it to be applied.
    void store(const std::string &json) {
        const auto read = Infra::Read(boost::json::parse(json));
        BOOST_TEST_REQUIRE(read.error.empty(), read.error);

        static int generation = 0;
        const auto internalName = "declaration-" + std::to_string(++generation);
        std::ofstream(scratch() / "esm" / internalName, std::ios::binary) << boost::json::serialize(Infra::Write(read.declaration));

        Euclid::Database::Entity::ESM::Object object;
        object.bucketErn = bucketErn();
        object.key = Infra::ObjectKey(kApplication);
        object.ern = Euclid::Core::createEsmObjectErn(kAccount, kNamespace, "apps/" + object.key);
        object.internalName = internalName;
        std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->upsertObject(object);
    }

    struct Answer {
        int status{};
        boost::json::object body;
    };

    Answer apply(boost::json::object request = {}, const std::string &action = "apply-infrastructure") {
        request["applicationId"] = kApplication;
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken("admin", kJwtSecret));
        req.set("x-euclid-target", "eap");
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", kNamespace);
        req.body() = boost::json::serialize(request);
        req.prepare_payload();

        const auto res = send(req);
        const auto parsed = boost::json::parse(res.body());
        return {static_cast<int>(res.result_int()), parsed.is_object() ? parsed.as_object() : boost::json::object{}};
    }

    std::optional<Euclid::Database::Entity::EQS::Queue> queue(const std::string &name) {
        return Euclid::Database::RepositoryFactory::instance().eqsRepository()->findQueueByName(kAccount, kNamespace, name);
    }

    Euclid::Database::Entity::EQS::Queue makeQueue(const std::string &name, const std::string &owner = {}) {
        Euclid::Database::Entity::EQS::Queue q;
        q.name = name;
        q.ern = queueErn(name);
        q.accountId = kAccount;
        q.nameSpace = kNamespace;
        q.region = kRegion;
        if (!owner.empty()) q.tags[Infra::kOwnerTag] = owner;
        return Euclid::Database::RepositoryFactory::instance().eqsRepository()->upsertQueue(q);
    }

    std::size_t size(const boost::json::object &body, const char *field) {
        const auto *value = body.if_contains(field);
        return value != nullptr && value->is_array() ? value->as_array().size() : 0;
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EapApplyInfrastructureTest, Fixture)

    // The bug this merge fixed: on the deploy path every setting was dropped.
    BOOST_AUTO_TEST_CASE(AQueueIsMadeWithTheSettingsItsFileNames) {
        store(R"({"version":1,"applicationId":"orders","creates":{"queues":[
                 {"name":"orders-in","visibility":900,"maxRetries":5,"priority":"HIGH","deadLetterQueue":"orders-dlq"},
                 {"name":"orders-dlq"}]}})");

        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));

        const auto in = queue("orders-in");
        BOOST_TEST_REQUIRE(in.has_value());
        BOOST_TEST(in->visibility == 900L);
        BOOST_TEST(in->maxReceiveCount == 5L);
        BOOST_TEST((in->priority == Euclid::Database::Entity::EQS::MessagePriority::HIGH));
        BOOST_TEST(in->deadLetterQueueErn == queueErn("orders-dlq"));
        BOOST_TEST(in->tags.at(Infra::kOwnerTag) == kApplication);
        BOOST_TEST(queue("orders-dlq").has_value());
    }

    // What every queue declared before settings were read needs: the file's value, applied to the
    // queue as it is.
    BOOST_AUTO_TEST_CASE(AnExistingQueueIsBroughtToItsDeclaredSettings) {
        makeQueue("orders-in", kApplication);
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in","visibility":600}]}})");

        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));
        BOOST_TEST(queue("orders-in")->visibility == 600L);
        BOOST_TEST(size(answer.body, "updated") == 1U);
        BOOST_TEST(size(answer.body, "created") == 0U);
    }

    BOOST_AUTO_TEST_CASE(ApplyingTwiceChangesNothingTheSecondTime) {
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in","visibility":600}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        const auto again = apply();
        BOOST_TEST_REQUIRE(again.status == 200);
        for (const auto *field: {"created", "adopted", "updated", "deleted", "kept", "granted", "revoked"}) {
            BOOST_TEST(size(again.body, field) == 0U, field);
        }
    }

    // The deploy path used to record such a queue as owned, for the owner's reconcile to delete.
    BOOST_AUTO_TEST_CASE(AQueueAnotherApplicationOwnsIsRefused) {
        makeQueue("orders-in", "billing");
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in","visibility":600}]}})");

        const auto answer = apply();
        BOOST_TEST(answer.status == 400);
        BOOST_TEST(boost::json::serialize(answer.body).find("owned by 'billing'") != std::string::npos);
        BOOST_TEST(queue("orders-in")->visibility == 30L);
        BOOST_TEST(queue("orders-in")->tags.at(Infra::kOwnerTag) == "billing");
    }

    BOOST_AUTO_TEST_CASE(AnUntaggedQueueIsAdopted) {
        makeQueue("orders-in");
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"}]}})");

        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200);
        BOOST_TEST(size(answer.body, "adopted") == 1U);
        BOOST_TEST(queue("orders-in")->tags.at(Infra::kOwnerTag) == kApplication);
    }

    // A dry run is the folder handed over, and nothing stored or changed.
    BOOST_AUTO_TEST_CASE(ADryRunChangesNothing) {
        const auto declaration = boost::json::parse(R"({"version":1,"creates":{"queues":[{"name":"orders-in","visibility":600}]},
                                                       "uses":{"queues":[{"name":"prices","access":"consume"}]}})");
        makeQueue("prices", "pricing");

        const auto answer = apply({{"declaration", declaration}}, "plan-infrastructure");
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));
        BOOST_TEST(size(answer.body, "created") == 1U);
        // Consume on the queue it uses, and everything on the one it would own.
        BOOST_TEST(size(answer.body, "granted") == 2U);
        BOOST_TEST(!queue("orders-in").has_value());
        BOOST_TEST(Euclid::Database::RepositoryFactory::instance().eamRepository()->findGrantsByPrincipals({principalErn()}).empty());
    }

    // Applied for real, a handed-over declaration would be undone by the next deploy.
    // And a dry-run flag on apply-infrastructure is not one - see handlePlanInfrastructure().
    BOOST_AUTO_TEST_CASE(ADeclarationIsOnlyHandedOverForADryRun) {
        const auto declaration = boost::json::parse(R"({"version":1,"creates":{"queues":[{"name":"orders-in"}]}})");
        BOOST_TEST(apply({{"declaration", declaration}}).status == 400);
    }

    // The rule `eap apply` always had, now the deploy's too: a line removed from a file is not a
    // decision to destroy what is in the queue.
    BOOST_AUTO_TEST_CASE(AQueueNoLongerDeclaredIsRemovedOnlyWhenEmpty) {
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"},{"name":"orders-old"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        std::ignore = Euclid::Database::RepositoryFactory::instance().eqsRepository()->sendMessage(
                "m-1", queueErn("orders-old") + ":m-1", queueErn("orders-old"), "still here", {}, {},
                Euclid::Database::Entity::EQS::MessagePriority::MEDIUM);

        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"}]}})");
        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));
        BOOST_TEST(size(answer.body, "kept") == 1U);
        BOOST_TEST(size(answer.body, "deleted") == 0U);
        BOOST_TEST(queue("orders-old").has_value());
    }

    BOOST_AUTO_TEST_CASE(AnEmptyQueueNoLongerDeclaredIsRemoved) {
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"},{"name":"orders-old"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"}]}})");
        const auto answer = apply();
        BOOST_TEST(size(answer.body, "deleted") == 1U);
        BOOST_TEST(!queue("orders-old").has_value());
    }

    BOOST_AUTO_TEST_CASE(WithoutPruningNothingIsRemoved) {
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"},{"name":"orders-old"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"}]}})");
        const auto answer = apply({{"prune", false}});
        BOOST_TEST(size(answer.body, "kept") == 1U);
        BOOST_TEST(queue("orders-old").has_value());
    }

    // Grants are made to match rather than replaced wholesale, and a grant made by hand survives.
    BOOST_AUTO_TEST_CASE(GrantsAreExactlyWhatTheFileAsksFor) {
        makeQueue("prices", "pricing");
        makeQueue("stock", "warehouse");

        Euclid::Database::Entity::EAM::Grant byHand;
        byHand.role = "pim-object-reader";
        byHand.principal = principalErn();
        byHand.accountId = kAccount;
        std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->addGrant(byHand);

        store(R"({"version":1,"uses":{"queues":[{"name":"prices","access":"consume"},{"name":"stock","access":"consume"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        store(R"({"version":1,"uses":{"queues":[{"name":"prices","access":"consume"}]}})");
        const auto answer = apply();
        BOOST_TEST(size(answer.body, "revoked") == 1U);
        BOOST_TEST(size(answer.body, "granted") == 0U);

        const auto grants = Euclid::Database::RepositoryFactory::instance().eamRepository()->findGrantsByPrincipals({principalErn()});
        BOOST_TEST(grants.size() == 2U);
        BOOST_TEST(std::ranges::any_of(grants, [](const auto &g) { return g.role == "pim-object-reader"; }));
    }

    BOOST_AUTO_TEST_CASE(ADeadLetterQueueThatIsNowhereIsRefused) {
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in","deadLetterQueue":"orders-nowhere"}]}})");
        const auto answer = apply();
        BOOST_TEST(answer.status == 400);
        BOOST_TEST(!queue("orders-in").has_value());
    }

    // Owning it is the access: granted on the object, not left to an `application` grant on "*",
    // which never covered deleting, moving or renaming in an application's own bucket.
    BOOST_AUTO_TEST_CASE(WhatAnApplicationCreatesItIsGrantedEverythingOn) {
        store(R"({"version":1,"creates":{"buckets":[{"name":"orders-work"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        const auto &eam = Euclid::Database::RepositoryFactory::instance().eamRepository();
        const auto grants = eam->findGrantsByPrincipals({principalErn()});
        BOOST_TEST_REQUIRE(grants.size() == 1U);
        BOOST_TEST(grants[0].role == "access-bucket-own");
        BOOST_TEST((grants[0].resources == std::vector<std::string>{Euclid::Core::createEsmBucketErn(kAccount, kNamespace, "orders-work")}));

        const auto role = eam->findRoleByName(kAccount, "access-bucket-own");
        BOOST_TEST_REQUIRE(role.has_value());
        for (const auto *permission: {"esm:put-object", "esm:get-object", "esm:delete-object", "esm:move-object", "esm:rename-object", "esm:subscribe"}) {
            BOOST_TEST(std::ranges::contains(role->permissions, permission), permission);
        }
    }

    BOOST_AUTO_TEST_CASE(WhatIsNoLongerOwnedIsNoLongerGranted) {
        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"},{"name":"orders-old"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"}]}})");
        const auto answer = apply();
        BOOST_TEST(size(answer.body, "revoked") == 1U);
        const auto grants = Euclid::Database::RepositoryFactory::instance().eamRepository()->findGrantsByPrincipals({principalErn()});
        BOOST_TEST_REQUIRE(grants.size() == 1U);
        BOOST_TEST((grants[0].resources == std::vector<std::string>{queueErn("orders-in")}));
    }

    // The levels that replace pim-object-remover, -copier, -renamer and pim-queue-switch.
    BOOST_AUTO_TEST_CASE(TheNewLevelsAreGrantedOnTheObjectNamed) {
        Euclid::Database::Entity::ESM::Bucket bucket;
        bucket.name = "transfer-server";
        bucket.ern = Euclid::Core::createEsmBucketErn(kAccount, kNamespace, "transfer-server");
        bucket.accountId = kAccount;
        bucket.nameSpace = kNamespace;
        std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->upsertBucket(bucket);
        makeQueue("orders-switch", "pricing");

        store(R"({"version":1,"uses":{"buckets":[{"name":"transfer-server","access":["read","delete","move","rename"]}],
                                      "queues":[{"name":"orders-switch","access":"control"}]}})");
        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));
        BOOST_TEST(size(answer.body, "granted") == 5U);

        const auto grants = Euclid::Database::RepositoryFactory::instance().eamRepository()->findGrantsByPrincipals({principalErn()});
        for (const auto *role: {"access-bucket-delete", "access-bucket-move", "access-bucket-rename", "access-queue-control"}) {
            BOOST_TEST(std::ranges::any_of(grants, [&](const auto &g) { return g.role == role; }), role);
        }
    }

    // ── tables ─────────────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(ATableIsMadeWithItsKeyAndGrantedToItsOwner) {
        store(R"({"version":1,"creates":{"tables":[{"name":"orders-state","partitionKey":"orderId","sortKey":"version","sortKeyType":"number"}]}})");
        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));

        const auto table = Euclid::Database::RepositoryFactory::instance().ekvRepository()->findTable(kAccount, kNamespace, "orders-state");
        BOOST_TEST_REQUIRE(table.has_value());
        BOOST_TEST(table->partitionKey.name == "orderId");
        BOOST_TEST_REQUIRE(table->sortKey.has_value());
        BOOST_TEST((table->sortKey->type == Euclid::Database::Entity::EKV::KeyType::Number));
        BOOST_TEST(table->tags.at(Infra::kOwnerTag) == kApplication);

        const auto grants = Euclid::Database::RepositoryFactory::instance().eamRepository()->findGrantsByPrincipals({principalErn()});
        BOOST_TEST_REQUIRE(grants.size() == 1U);
        BOOST_TEST(grants[0].role == "access-table-own");
    }

    // A key cannot change once items are keyed on it; a file that says otherwise is wrong about the table.
    BOOST_AUTO_TEST_CASE(ATableKeyedDifferentlyIsRefused) {
        store(R"({"version":1,"creates":{"tables":[{"name":"orders-state","partitionKey":"orderId"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        store(R"({"version":1,"creates":{"tables":[{"name":"orders-state","partitionKey":"id"}]}})");
        const auto answer = apply();
        BOOST_TEST(answer.status == 400);
        BOOST_TEST(boost::json::serialize(answer.body).find("cannot change") != std::string::npos);
    }

    // What every table the script made is on the first apply: there, untagged, and claimed.
    BOOST_AUTO_TEST_CASE(AnUntaggedTableIsAdopted) {
        Euclid::Database::Entity::EKV::Table table;
        table.name = "datenlieferant";
        table.accountId = kAccount;
        table.nameSpace = kNamespace;
        table.ern = Euclid::Core::createEkvTableErn(kAccount, kNamespace, "datenlieferant");
        table.partitionKey = {.name = "datenlieferantId", .type = Euclid::Database::Entity::EKV::KeyType::String};
        std::ignore = Euclid::Database::RepositoryFactory::instance().ekvRepository()->createTable(table);

        store(R"({"version":1,"creates":{"tables":[{"name":"datenlieferant","partitionKey":"datenlieferantId"}]}})");
        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));
        BOOST_TEST(size(answer.body, "adopted") == 1U);
        BOOST_TEST(Euclid::Database::RepositoryFactory::instance().ekvRepository()->findTable(kAccount, kNamespace, "datenlieferant")->tags.at(Infra::kOwnerTag) == kApplication);
    }

    BOOST_AUTO_TEST_CASE(ATableWithItemsIsKeptWhenNoLongerDeclared) {
        store(R"({"version":1,"creates":{"tables":[{"name":"orders-state","partitionKey":"orderId"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);

        const auto repository = Euclid::Database::RepositoryFactory::instance().ekvRepository();
        const auto table = repository->findTable(kAccount, kNamespace, "orders-state");
        std::string error;
        auto item = Euclid::Database::Entity::EKV::Item::FromAttributes(
                *table, {{"orderId", Euclid::Database::Entity::EKV::Value(std::string("o-1"))}}, error);
        BOOST_TEST_REQUIRE(item.has_value(), error);
        std::ignore = repository->putItem(*item);

        store(R"({"version":1,"creates":{"queues":[{"name":"orders-in"}]}})");
        const auto answer = apply();
        BOOST_TEST(size(answer.body, "kept") == 1U);
        BOOST_TEST(repository->findTable(kAccount, kNamespace, "orders-state").has_value());
    }

    // ── routes ─────────────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(ARouteIsPublishedForTheApplicationDeclaringIt) {
        store(R"({"version":1,"creates":{"routes":[{"name":"orders-api","path":"/api/orders","methods":["GET","post"],"authentication":"EUCLID","timeout":300}]}})");
        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));

        const auto route = Euclid::Database::RepositoryFactory::instance().eagRepository()->findRouteByRouteId(kAccount, kNamespace, "orders-api");
        BOOST_TEST_REQUIRE(route.has_value());
        BOOST_TEST(route->applicationId == kApplication);
        BOOST_TEST(route->path == "/api/orders");
        BOOST_TEST((route->methods == std::vector<std::string>{"GET", "POST"}));
        BOOST_TEST((route->authentication == Euclid::Database::Entity::EAG::RouteAuthentication::EUCLID));
        BOOST_TEST(route->timeout == 300L);
        // A route is reached over HTTP, not through a permission: nothing is granted for it.
        BOOST_TEST(Euclid::Database::RepositoryFactory::instance().eamRepository()->findGrantsByPrincipals({principalErn()}).empty());
    }

    BOOST_AUTO_TEST_CASE(ARoutesDeclaredSettingsAreBroughtIntoLine) {
        store(R"({"version":1,"creates":{"routes":[{"name":"orders-api","path":"/api/orders"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);
        store(R"({"version":1,"creates":{"routes":[{"name":"orders-api","path":"/api/orders","timeout":600}]}})");
        const auto answer = apply();
        BOOST_TEST(size(answer.body, "updated") == 1U);
        BOOST_TEST(Euclid::Database::RepositoryFactory::instance().eagRepository()->findRouteByRouteId(kAccount, kNamespace, "orders-api")->timeout == 600L);
    }

    BOOST_AUTO_TEST_CASE(ARouteToAnotherApplicationIsRefused) {
        Euclid::Database::Entity::EAG::Route route;
        route.routeId = "orders-api";
        route.path = "/api/orders";
        route.applicationId = "billing";
        route.accountId = kAccount;
        route.nameSpace = kNamespace;
        std::ignore = Euclid::Database::RepositoryFactory::instance().eagRepository()->upsertRoute(route);

        store(R"({"version":1,"creates":{"routes":[{"name":"orders-api","path":"/api/orders"}]}})");
        const auto answer = apply();
        BOOST_TEST(answer.status == 400);
        BOOST_TEST(boost::json::serialize(answer.body).find("owned by 'billing'") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(APathAnotherRouteServesIsRefused) {
        Euclid::Database::Entity::EAG::Route route;
        route.routeId = "billing-api";
        route.path = "/api/orders";
        route.applicationId = "billing";
        route.accountId = kAccount;
        route.nameSpace = kNamespace;
        std::ignore = Euclid::Database::RepositoryFactory::instance().eagRepository()->upsertRoute(route);

        store(R"({"version":1,"creates":{"routes":[{"name":"orders-api","path":"/api/orders"}]}})");
        const auto answer = apply();
        BOOST_TEST(answer.status == 400);
        BOOST_TEST(boost::json::serialize(answer.body).find("already routed by 'billing-api'") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(ARouteNoLongerDeclaredIsRemoved) {
        store(R"({"version":1,"creates":{"routes":[{"name":"orders-api","path":"/api/orders"},{"name":"orders-old","path":"/api/old"}]}})");
        BOOST_TEST_REQUIRE(apply().status == 200);
        store(R"({"version":1,"creates":{"routes":[{"name":"orders-api","path":"/api/orders"}]}})");
        const auto answer = apply();
        BOOST_TEST(size(answer.body, "deleted") == 1U);
        BOOST_TEST(!Euclid::Database::RepositoryFactory::instance().eagRepository()->findRouteByRouteId(kAccount, kNamespace, "orders-old").has_value());
    }

    // ── platforms ──────────────────────────────────────────────────────────

    namespace {
        Answer platform(const std::string &declaration, const std::string &action = "apply-platform", boost::json::object extra = {}) {
            extra["name"] = "shared";
            extra["declaration"] = boost::json::parse(declaration);
            http::request<http::string_body> req(http::verb::post, "/", 11);
            req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken("admin", kJwtSecret));
            req.set("x-euclid-target", "eap");
            req.set("x-euclid-action", action);
            req.set("x-euclid-account-id", kAccount);
            req.set("x-euclid-namespace", kNamespace);
            req.body() = boost::json::serialize(extra);
            req.prepare_payload();
            const auto res = send(req);
            const auto parsed = boost::json::parse(res.body());
            return {static_cast<int>(res.result_int()), parsed.is_object() ? parsed.as_object() : boost::json::object{}};
        }

        std::optional<Euclid::Database::Entity::ESM::Bucket> bucket(const std::string &name) {
            return Euclid::Database::RepositoryFactory::instance().esmRepository()->findBucketByName(kAccount, kNamespace, name);
        }
    }// namespace

    // The bucket an FTP server writes into belongs to no application.
    BOOST_AUTO_TEST_CASE(APlatformOwnsWhatNoApplicationDoes) {
        const auto answer = platform(R"({"version":1,"creates":{"buckets":[{"name":"transfer-server"}],"topics":[{"name":"reminder-topic"}]}})");
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));
        BOOST_TEST(size(answer.body, "created") == 2U);
        BOOST_TEST(bucket("transfer-server")->tags.at(Infra::kOwnerTag) == "platform:shared");
        // A platform has no principal: nothing is granted for what it owns.
        BOOST_TEST(size(answer.body, "granted") == 0U);
    }

    // What makes a platform object usable: an application reaches it like anybody else's.
    BOOST_AUTO_TEST_CASE(AnApplicationUsesAPlatformsObject) {
        BOOST_TEST_REQUIRE(platform(R"({"version":1,"creates":{"buckets":[{"name":"transfer-server"}]}})").status == 200);
        store(R"({"version":1,"uses":{"buckets":[{"name":"transfer-server","access":["read","move"],"owner":"platform"}]}})");
        const auto answer = apply();
        BOOST_TEST_REQUIRE(answer.status == 200, boost::json::serialize(answer.body));
        BOOST_TEST(size(answer.body, "granted") == 2U);
    }

    BOOST_AUTO_TEST_CASE(AnApplicationCannotTakeOverAPlatformsObject) {
        BOOST_TEST_REQUIRE(platform(R"({"version":1,"creates":{"buckets":[{"name":"transfer-server"}]}})").status == 200);
        store(R"({"version":1,"creates":{"buckets":[{"name":"transfer-server"}]}})");
        const auto answer = apply();
        BOOST_TEST(answer.status == 400);
        BOOST_TEST(boost::json::serialize(answer.body).find("owned by 'platform:shared'") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(APlatformOnlyCreates) {
        BOOST_TEST(platform(R"({"version":1,"uses":{"buckets":[{"name":"b","access":"read"}]}})").status == 400);
        BOOST_TEST(platform(R"({"version":1,"creates":{"routes":[{"name":"r","path":"/p"}]}})").status == 400);
        BOOST_TEST(platform(R"({"version":1,"applicationId":"orders","creates":{"buckets":[{"name":"b"}]}})").status == 400);
    }

    BOOST_AUTO_TEST_CASE(APlatformsDryRunChangesNothing) {
        const auto answer = platform(R"({"version":1,"creates":{"buckets":[{"name":"transfer-server"}]}})", "plan-platform");
        BOOST_TEST_REQUIRE(answer.status == 200);
        BOOST_TEST(size(answer.body, "created") == 1U);
        BOOST_TEST(!bucket("transfer-server").has_value());
    }

    // The record is what the next apply diffs against, so what a platform stops declaring goes - empty.
    BOOST_AUTO_TEST_CASE(APlatformRemovesWhatItNoLongerDeclares) {
        BOOST_TEST_REQUIRE(platform(R"({"version":1,"creates":{"buckets":[{"name":"transfer-server"},{"name":"old-drop"}]}})").status == 200);
        const auto answer = platform(R"({"version":1,"creates":{"buckets":[{"name":"transfer-server"}]}})");
        BOOST_TEST_REQUIRE(answer.status == 200);
        BOOST_TEST(size(answer.body, "deleted") == 1U);
        BOOST_TEST(!bucket("old-drop").has_value());
    }

BOOST_AUTO_TEST_SUITE_END()
