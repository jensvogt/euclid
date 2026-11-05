// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmDownloadGrantTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <map>
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
#include <euclid/database/RepositoryFactory.h>

// Who may download what. create-download was the one read of an object that checked no grant -
// get-object did, and every object over the inline limit is read through create-download instead -
// and download-part served any download to whoever held its ID. And the upload and download IDs
// were joined onto paths unchecked, so complete-download removed whatever directory "../" named.
//
// Driven through the server's socket with the real authorization lookup wired, as a principal
// holding the built-in node role - exactly the four download actions - on one bucket only.

namespace {

    namespace http = boost::beast::http;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-download-grant-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    std::string bucketErn(const std::string &name) { return Euclid::Core::createEsmBucketErn(kAccount, kNamespace, name); }
    std::string userErn(const std::string &userId) { return std::string("ern:eam:") + kRegion + ":" + kAccount + ":user:" + userId; }

    Euclid::ESM::EsmServer &server() {
        static Euclid::ESM::EsmServer *instance = [] {
            std::filesystem::create_directories(scratch() / "esm");
            auto *created = new Euclid::ESM::EsmServer((scratch() / "esm.sock").string(), 2);
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

    void bucket(const std::string &name) {
        Euclid::Database::Entity::ESM::Bucket b;
        b.name = name;
        b.ern = bucketErn(name);
        b.accountId = kAccount;
        b.nameSpace = kNamespace;
        b.region = kRegion;
        std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->upsertBucket(b);
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

    Answer send(const std::string &as, const std::string &action, const std::string &body, const std::map<std::string, std::string> &headers = {}) {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + Euclid::Core::JwtUtils::CreateToken(as, kJwtSecret));
        req.set("x-euclid-target", "esm");
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", kAccount);
        req.set("x-euclid-namespace", kNamespace);
        req.set("x-euclid-user-id", as);
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

    Answer ask(const std::string &as, const std::string &action, const boost::json::object &body) {
        return send(as, action, boost::json::serialize(body));
    }

    Answer createDownload(const std::string &as, const std::string &bucketName, const std::string &key) {
        return ask(as, "create-download", {{"bucketErn", bucketErn(bucketName)}, {"key", key}});
    }

    Answer downloadPart(const std::string &as, const std::string &downloadId) {
        return send(as, "download-part", "", {{"x-euclid-download-id", downloadId}, {"x-euclid-part-number", "1"}, {"x-euclid-part-size", "1048576"}});
    }

    struct Fixture {
        Fixture() {
            auto &configuration = Euclid::Core::Configuration::instance();
            configuration.set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
            configuration.set<std::string>("euclid.modules.esm.data-dir", (scratch() / "esm").string());
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

            // A worker node's principal, allowed to fetch from one bucket and no other.
            user("fetcher");
            Euclid::Database::Entity::EAM::Grant grant;
            grant.role = "node";
            grant.principal = userErn("fetcher");
            grant.accountId = kAccount;
            grant.namespaces = {kNamespace};
            grant.resources = {bucketErn("allowed")};
            std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->addGrant(grant);

            bucket("allowed");
            bucket("denied");
            for (const auto *name: {"allowed", "denied"}) {
                BOOST_REQUIRE(send("admin", "put-object", "artifact bytes", {{"x-euclid-bucket-ern", bucketErn(name)}, {"x-euclid-key", "app.jar"}}).status == 200);
            }
        }
    };

}// namespace

BOOST_FIXTURE_TEST_CASE(ADownloadNeedsTheBucketGranted, Fixture) {
    BOOST_TEST(createDownload("fetcher", "denied", "app.jar").status == 403);

    const auto allowed = createDownload("fetcher", "allowed", "app.jar");
    BOOST_REQUIRE(allowed.status == 200);
    const auto downloadId = std::string(allowed.json().at("downloadId").as_string());

    const auto part = downloadPart("fetcher", downloadId);
    BOOST_TEST(part.status == 200);
    BOOST_TEST(part.body == "artifact bytes");
    BOOST_TEST(ask("fetcher", "complete-download", {{"downloadId", downloadId}}).status == 200);
}

BOOST_FIXTURE_TEST_CASE(ABucketInAnotherAccountIsRefused, Fixture) {
    Euclid::Database::Entity::ESM::Bucket foreign;
    foreign.name = "foreign";
    foreign.ern = Euclid::Core::createEsmBucketErn("111111111111", kNamespace, "foreign");
    foreign.accountId = "111111111111";
    foreign.nameSpace = kNamespace;
    foreign.region = kRegion;
    std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->upsertBucket(foreign);

    BOOST_TEST(ask("admin", "create-download", {{"bucketErn", foreign.ern}, {"key", "app.jar"}}).status == 403);
}

BOOST_FIXTURE_TEST_CASE(ADownloadIsOnlyServedToWhoeverStartedIt, Fixture) {
    // Somebody else's download ID, of a bucket the fetcher may read: still not the fetcher's.
    const auto started = createDownload("admin", "allowed", "app.jar");
    BOOST_REQUIRE(started.status == 200);
    const auto downloadId = std::string(started.json().at("downloadId").as_string());

    BOOST_TEST(downloadPart("fetcher", downloadId).status == 403);
    BOOST_TEST(ask("fetcher", "complete-download", {{"downloadId", downloadId}}).status == 403);

    // Refused without being ended: the one who started it still has it.
    BOOST_TEST(downloadPart("admin", downloadId).status == 200);
    BOOST_TEST(ask("admin", "complete-download", {{"downloadId", downloadId}}).status == 200);
}

BOOST_FIXTURE_TEST_CASE(ADownloadIdIsNotAPath, Fixture) {
    // The object store, one level up from where downloads are kept. complete-download used to
    // remove_all() it.
    const auto objects = scratch() / "esm" / "objects";
    std::filesystem::create_directories(objects);
    std::ofstream(objects / "canary") << "still here";

    for (const auto *id: {"../objects", "../../esm", "..", "6f1b1e2e-0000-0000-0000-00000000000/"}) {
        BOOST_TEST(ask("admin", "complete-download", {{"downloadId", id}}).status == 400);
        BOOST_TEST(downloadPart("admin", id).status == 400);
    }
    BOOST_TEST(std::filesystem::exists(objects / "canary"));
}

BOOST_FIXTURE_TEST_CASE(AnUploadIdIsNotAPath, Fixture) {
    BOOST_TEST(send("admin", "upload-part", "x", {{"x-euclid-upload-id", "../objects"}, {"x-euclid-part-number", "1"}}).status == 400);
    BOOST_TEST(ask("admin", "complete-upload", {{"uploadId", "../objects"}}).status == 400);
    BOOST_TEST(ask("admin", "abort-upload", {{"uploadId", "../objects"}}).status == 400);
}

BOOST_FIXTURE_TEST_CASE(AnUnknownButWellFormedIdIsNotFound, Fixture) {
    // The check is on the shape of the ID, not a refusal of everything: a real-looking ID that
    // names nothing answers as it always has.
    BOOST_TEST(downloadPart("admin", "6f1b1e2e-3a21-4e6b-9a0c-5f2b9e1d4c33").status == 404);
    BOOST_TEST(ask("admin", "complete-download", {{"downloadId", "6f1b1e2e-3a21-4e6b-9a0c-5f2b9e1d4c33"}}).status == 200);
}
