// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EdsUpdateTest
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
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <EdsServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>

// eds:update-zone and eds:update-record, driven through the module's socket against the in-memory
// store. For a zone: only what the request carries changes, an empty description clears, nothing at
// all or a TTL of 0 is refused, and another account's zone is as absent as one that does not exist.
// For a record set: values are removed before they are added, compared encoded rather than as typed,
// a value to remove has to be there, and no change may leave the set empty or a CNAME with two.

namespace {

    namespace http = boost::beast::http;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kOtherAccount = "111111111111";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";
    constexpr auto kUser = "zone-admin";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-eds-update-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    unsigned short freePort() {
        boost::asio::io_context context;
        boost::asio::ip::udp::socket probe(context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
        return probe.local_endpoint().port();
    }

    Euclid::EDS::EdsServer &server() {
        static Euclid::EDS::EdsServer *instance = [] {
            std::filesystem::create_directories(scratch());
            auto &configuration = Euclid::Core::Configuration::instance();
            // The DNS port is not what is tested; a free one on loopback keeps it from colliding
            // with a real EDS on this host, and no service names keeps the zone reload to the store.
            configuration.set<std::string>("euclid.modules.eds.address", "127.0.0.1");
            configuration.set<long>("euclid.modules.eds.port", freePort());
            configuration.set<std::string>("euclid.modules.eds.service-domain", "");
            auto *created = new Euclid::EDS::EdsServer((scratch() / "eds.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    std::string userErn(const std::string &userId) { return std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:" + userId; }

    struct Fixture {
        Fixture() {
            Euclid::Core::Configuration::instance().set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            Euclid::Database::Database::instance().initializeMemory();
            std::ignore = server();
            Euclid::Database::WireAuthorizationLookup();

            const auto eam = Euclid::Database::RepositoryFactory::instance().eamRepository();

            Euclid::Database::Entity::EAM::Role role;
            role.name = "eds-admin";
            role.accountId = kAccount;
            role.permissions = {"eds:create-zone", "eds:get-zone", "eds:update-zone", "eds:delete-zone",
                                "eds:put-record", "eds:update-record", "eds:list-records", "eds:get-answer"};
            std::ignore = eam->upsertRole(role);

            Euclid::Database::Entity::EAM::User user;
            user.userId = kUser;
            user.email = std::string(kUser) + "@example.org";
            user.accountId = kAccount;
            user.region = kRegion;
            user.ern = userErn(kUser);
            std::ignore = eam->upsertUser(user);

            Euclid::Database::Entity::EAM::Grant grant;
            grant.role = role.name;
            grant.principal = userErn(kUser);
            grant.accountId = kAccount;
            grant.namespaces = {kNamespace};
            grant.resources = {"*"};
            std::ignore = eam->addGrant(grant);
        }
    };

    std::pair<int, boost::json::value> call(const std::string &action, const boost::json::object &body) {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken(kUser, kJwtSecret));
        req.set("x-euclid-target", "eds");
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", kNamespace);
        req.set("x-euclid-user-id", kUser);
        req.body() = boost::json::serialize(body);
        req.prepare_payload();

        boost::asio::io_context ioc;
        boost::asio::local::stream_protocol::socket socket(ioc);
        socket.connect(boost::asio::local::stream_protocol::endpoint((scratch() / "eds.sock").string()));
        http::write(socket, req);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);

        boost::system::error_code ec;
        auto parsed = boost::json::parse(res.body(), ec);
        return {static_cast<int>(res.result_int()), ec ? boost::json::value{} : std::move(parsed)};
    }

    // A zone of its own per test, so the cases do not depend on the order they run in.
    std::string zone(const std::string &name, const long ttl, const std::string &description) {
        const auto [status, body] = call("create-zone", {{"name", name}, {"ttl", ttl}, {"description", description}});
        BOOST_TEST_REQUIRE(status == 201);
        return name;
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EdsZoneUpdate, Fixture)

    BOOST_AUTO_TEST_CASE(OnlyTheTtlChangesWhenOnlyTheTtlIsGiven) {
        const auto name = zone("ttl-only.example", 120, "office");

        const auto [status, body] = call("update-zone", {{"name", name}, {"ttl", 60}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("ttl").as_int64() == 60);
        BOOST_TEST(body.at("description").as_string() == "office");

        // And it is stored, not just echoed.
        const auto [getStatus, stored] = call("get-zone", {{"name", name}});
        BOOST_TEST_REQUIRE(getStatus == 200);
        BOOST_TEST(stored.at("ttl").as_int64() == 60);
        BOOST_TEST(stored.at("description").as_string() == "office");
    }

    BOOST_AUTO_TEST_CASE(OnlyTheDescriptionChangesWhenOnlyItIsGiven) {
        const auto name = zone("description-only.example", 120, "old");

        const auto [status, body] = call("update-zone", {{"name", name}, {"description", "new"}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("ttl").as_int64() == 120);
        BOOST_TEST(body.at("description").as_string() == "new");
    }

    BOOST_AUTO_TEST_CASE(AnEmptyDescriptionClearsIt) {
        const auto name = zone("clear.example", 120, "something");

        const auto [status, body] = call("update-zone", {{"name", name}, {"description", ""}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("description").as_string().empty());
        BOOST_TEST(body.at("ttl").as_int64() == 120);
    }

    BOOST_AUTO_TEST_CASE(TheSerialDoesNotGoBackwards) {
        const auto name = zone("serial.example", 120, "");
        const auto before = call("get-zone", {{"name", name}}).second.at("serial").as_int64();

        const auto [status, body] = call("update-zone", {{"name", name}, {"ttl", 30}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("serial").as_int64() >= before);
    }

    BOOST_AUTO_TEST_CASE(NothingToChangeIsRefused) {
        const auto name = zone("nothing.example", 120, "");
        BOOST_TEST(call("update-zone", {{"name", name}}).first == 400);
    }

    BOOST_AUTO_TEST_CASE(ATtlOfZeroIsRefused) {
        const auto name = zone("zero.example", 120, "");
        BOOST_TEST(call("update-zone", {{"name", name}, {"ttl", 0}}).first == 400);
        BOOST_TEST(call("update-zone", {{"name", name}, {"ttl", -5}}).first == 400);
        // Unchanged.
        BOOST_TEST(call("get-zone", {{"name", name}}).second.at("ttl").as_int64() == 120);
    }

    BOOST_AUTO_TEST_CASE(AMissingZoneIsNotFound) {
        BOOST_TEST(call("update-zone", {{"name", "no-such-zone.example"}, {"ttl", 60}}).first == 404);
    }

    BOOST_AUTO_TEST_CASE(AnotherAccountsZoneIsNotFoundAndNotChanged) {
        Euclid::Database::Entity::EDS::HostedZone foreign;
        foreign.name = "foreign.example";
        foreign.accountId = kOtherAccount;
        foreign.nameSpace = kNamespace;
        foreign.region = kRegion;
        foreign.ern = Euclid::Core::createEdsZoneErn(kOtherAccount, kNamespace, foreign.name);
        foreign.ttl = 300;
        foreign.description = "theirs";
        foreign.created = foreign.modified = std::chrono::system_clock::now();
        const auto repository = Euclid::Database::RepositoryFactory::instance().edsRepository();
        std::ignore = repository->createZone(foreign);

        BOOST_TEST(call("update-zone", {{"name", foreign.name}, {"ttl", 5}, {"description", "mine now"}}).first == 404);

        const auto stored = repository->findZone(foreign.name);
        BOOST_TEST_REQUIRE(stored.has_value());
        BOOST_TEST(stored->ttl == 300);
        BOOST_TEST(stored->description == "theirs");
    }

    // ── Records ──────────────────────────────────────────────────────────────

    // The values stored at one name and type, as list-records gives them.
    std::vector<std::string> valuesOf(const std::string &zoneName, const std::string &name, const std::string &type) {
        const auto [status, body] = call("list-records", {{"zone", zoneName}, {"name", name}, {"type", type}});
        BOOST_TEST_REQUIRE(status == 200);
        std::vector<std::string> values;
        for (const auto &record: body.at("records").as_array()) {
            for (const auto &value: record.at("values").as_array()) values.emplace_back(value.as_string());
        }
        return values;
    }

    void put(const std::string &zoneName, const std::string &name, const std::string &type, const std::vector<std::string> &values, const long ttl = 0) {
        boost::json::array array;
        for (const auto &value: values) array.emplace_back(value);
        BOOST_TEST_REQUIRE(call("put-record", {{"zone", zoneName}, {"name", name}, {"type", type}, {"values", array}, {"ttl", ttl}}).first == 200);
    }

    boost::json::array list(const std::vector<std::string> &values) {
        boost::json::array array;
        for (const auto &value: values) array.emplace_back(value);
        return array;
    }

    BOOST_AUTO_TEST_CASE(AValueIsAddedWithoutResendingTheOthers) {
        const auto z = zone("add.example", 300, "");
        put(z, "api", "A", {"10.0.0.1", "10.0.0.2"});

        const auto [status, body] = call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"addValues", list({"10.0.0.3"})}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(valuesOf(z, "api", "A") == (std::vector<std::string>{"10.0.0.1", "10.0.0.2", "10.0.0.3"}));

        // And the server answers with it.
        const auto answer = call("get-answer", {{"name", "api.add.example"}, {"type", "A"}}).second;
        BOOST_TEST(answer.at("answers").as_array().size() == 3u);
    }

    BOOST_AUTO_TEST_CASE(AValueAlreadyThereIsNotAddedTwice) {
        const auto z = zone("dup.example", 300, "");
        put(z, "api", "A", {"10.0.0.1"});
        BOOST_TEST_REQUIRE(call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"addValues", list({" 10.0.0.1"})}}).first == 200);
        BOOST_TEST(valuesOf(z, "api", "A").size() == 1u);
    }

    BOOST_AUTO_TEST_CASE(RemovalComesFirstSoOneCallSwapsAValue) {
        const auto z = zone("swap.example", 300, "");
        put(z, "api", "A", {"10.0.0.1", "10.0.0.2"});

        const auto [status, body] = call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"},
                                                           {"removeValues", list({"10.0.0.1"})}, {"addValues", list({"10.0.0.9"})}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(valuesOf(z, "api", "A") == (std::vector<std::string>{"10.0.0.2", "10.0.0.9"}));
    }

    BOOST_AUTO_TEST_CASE(ValuesAreComparedEncodedNotAsTyped) {
        const auto z = zone("case.example", 300, "");
        put(z, "@", "MX", {"10 mail.case.example", "20 backup.case.example"});
        BOOST_TEST_REQUIRE(call("update-record", {{"zone", z}, {"name", "@"}, {"type", "mx"}, {"removeValues", list({"10   MAIL.Case.Example."})}}).first == 200);
        BOOST_TEST(valuesOf(z, "@", "MX") == (std::vector<std::string>{"20 backup.case.example"}));
    }

    BOOST_AUTO_TEST_CASE(AValueThatIsNotThereIsNotSilentlyIgnored) {
        const auto z = zone("typo.example", 300, "");
        put(z, "api", "A", {"10.0.0.1", "10.0.0.2"});
        BOOST_TEST(call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"removeValues", list({"10.0.0.7"})}}).first == 400);
        BOOST_TEST(valuesOf(z, "api", "A").size() == 2u);
    }

    BOOST_AUTO_TEST_CASE(TheLastValueCannotBeRemovedThisWay) {
        const auto z = zone("last.example", 300, "");
        put(z, "api", "A", {"10.0.0.1"});
        BOOST_TEST(call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"removeValues", list({"10.0.0.1"})}}).first == 400);
        BOOST_TEST(valuesOf(z, "api", "A").size() == 1u);
    }

    BOOST_AUTO_TEST_CASE(OnlyTheTtlChangesWhenOnlyItIsGiven) {
        const auto z = zone("rttl.example", 300, "");
        put(z, "api", "A", {"10.0.0.1"}, 60);

        auto [status, body] = call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"ttl", 30}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("ttl").as_int64() == 30);
        BOOST_TEST(valuesOf(z, "api", "A") == (std::vector<std::string>{"10.0.0.1"}));

        // 0 puts it back to the zone's.
        std::tie(status, body) = call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"ttl", 0}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("ttl").as_int64() == 0);
        const auto answer = call("get-answer", {{"name", "api.rttl.example"}, {"type", "A"}}).second;
        BOOST_TEST(answer.at("answers").as_array().at(0).at("ttl").as_int64() == 300);
    }

    BOOST_AUTO_TEST_CASE(ACnameKeepsExactlyOneValue) {
        const auto z = zone("alias.example", 300, "");
        put(z, "www", "CNAME", {"one.alias.example"});
        BOOST_TEST(call("update-record", {{"zone", z}, {"name", "www"}, {"type", "CNAME"}, {"addValues", list({"two.alias.example"})}}).first == 400);

        const auto [status, body] = call("update-record", {{"zone", z}, {"name", "www"}, {"type", "CNAME"},
                                                           {"removeValues", list({"one.alias.example"})}, {"addValues", list({"two.alias.example"})}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(valuesOf(z, "www", "CNAME") == (std::vector<std::string>{"two.alias.example"}));
    }

    BOOST_AUTO_TEST_CASE(AnUnencodableValueIsRefusedAndNothingChanges) {
        const auto z = zone("bad.example", 300, "");
        put(z, "api", "A", {"10.0.0.1"});
        BOOST_TEST(call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"addValues", list({"10.0.0.2", "not-an-address"})}}).first == 400);
        BOOST_TEST(valuesOf(z, "api", "A") == (std::vector<std::string>{"10.0.0.1"}));
    }

    BOOST_AUTO_TEST_CASE(AMissingRecordSetIsNotFound) {
        const auto z = zone("norecord.example", 300, "");
        BOOST_TEST(call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}, {"addValues", list({"10.0.0.1"})}}).first == 404);
    }

    BOOST_AUTO_TEST_CASE(NothingToChangeIsRefusedForARecordToo) {
        const auto z = zone("rnothing.example", 300, "");
        put(z, "api", "A", {"10.0.0.1"});
        BOOST_TEST(call("update-record", {{"zone", z}, {"name", "api"}, {"type", "A"}}).first == 400);
    }

BOOST_AUTO_TEST_SUITE_END()
