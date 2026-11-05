// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmStorageDirsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Boost includes
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <EsmServer.h>
#include <StorageDirs.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DirUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>

// euclid.modules.esm.storage-dirs: new objects spread round-robin over several directories -
// typically one per filesystem - each recording the one it went to as Object::internalDir, and
// every read, copy and delete finding the file there. Without the setting, the data directory as
// before and no directory recorded.

namespace {

    namespace http = boost::beast::http;
    using Euclid::ESM::ChooseStorageDir;
    using Euclid::ESM::ObjectRoot;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";
    constexpr auto kBody = "hello, world";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-storage-dirs-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    std::filesystem::path dataDir() { return scratch() / "esm"; }
    std::filesystem::path volumeA() { return scratch() / "volume-a"; }
    std::filesystem::path volumeB() { return scratch() / "volume-b"; }

    std::string bucketErn() { return Euclid::Core::createEsmBucketErn(kAccount, kNamespace, "incoming"); }

    Euclid::ESM::EsmServer &server() {
        static Euclid::ESM::EsmServer *instance = [] {
            std::filesystem::create_directories(dataDir());
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
            configuration.set<std::string>("euclid.modules.esm.data-dir", dataDir().string());
            configuration.set<std::vector<std::string> >("euclid.modules.esm.storage-dirs", {volumeA().string(), volumeB().string()});
            // The scratch directories share whatever filesystem /tmp is on, which may well have
            // less than the default gigabyte free.
            configuration.set<long>("euclid.modules.esm.storage-min-free-mb", 0);
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

    std::pair<int, std::string> call(const std::string &action, const std::string &body, const std::map<std::string, std::string> &headers = {}) {
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

    int put(const std::string &key) {
        return call("put-object", kBody, {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", key}}).first;
    }

    std::pair<int, std::string> get(const std::string &key) {
        return call("get-object", "", {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", key}, {"x-euclid-part-size", "1048576"}});
    }

    Euclid::Database::Entity::ESM::Object stored(const std::string &key) {
        const auto object = Euclid::Database::RepositoryFactory::instance().esmRepository()->findObjectByBucketAndKey(bucketErn(), key);
        BOOST_TEST_REQUIRE(object.has_value());
        return *object;
    }

    bool onDisk(const Euclid::Database::Entity::ESM::Object &object) {
        std::error_code ec;
        return std::filesystem::exists(Euclid::Core::DirUtils::FindFilePath(ObjectRoot(object.internalDir, dataDir()), object.internalName), ec);
    }

    // Free bytes by directory, for ChooseStorageDir(); a directory not in the map cannot be measured.
    Euclid::ESM::AvailableBytes freeSpace(std::map<std::string, std::uintmax_t> bytes) {
        return [bytes = std::move(bytes)](const std::filesystem::path &dir) -> std::optional<std::uintmax_t> {
            const auto found = bytes.find(dir.string());
            if (found == bytes.end()) return std::nullopt;
            return found->second;
        };
    }

    const std::vector<std::filesystem::path> kDirs{"/a", "/b", "/c"};

}// namespace

BOOST_AUTO_TEST_SUITE(Choosing)

    BOOST_AUTO_TEST_CASE(TakesTheDirectoriesInTurn) {
        const auto space = freeSpace({{"/a", 100}, {"/b", 100}, {"/c", 100}});
        BOOST_TEST(ChooseStorageDir(kDirs, 0, 10, 0, space).value() == 0U);
        BOOST_TEST(ChooseStorageDir(kDirs, 1, 10, 0, space).value() == 1U);
        BOOST_TEST(ChooseStorageDir(kDirs, 2, 10, 0, space).value() == 2U);
    }

    BOOST_AUTO_TEST_CASE(PassesOverAFilesystemWithoutRoom) {
        // /b has room for the object but not for the object and the margin.
        const auto space = freeSpace({{"/a", 100}, {"/b", 50}, {"/c", 100}});
        BOOST_TEST(ChooseStorageDir(kDirs, 1, 10, 45, space).value() == 2U);
    }

    BOOST_AUTO_TEST_CASE(PassesOverADirectoryThatCannotBeMeasured) {
        const auto space = freeSpace({{"/a", 100}, {"/c", 100}});
        BOOST_TEST(ChooseStorageDir(kDirs, 1, 10, 0, space).value() == 2U);
    }

    BOOST_AUTO_TEST_CASE(TakesTheRoomiestWhenNoneHasRoom) {
        // The write then fails with the disk's own reason, rather than being refused up front.
        const auto space = freeSpace({{"/a", 5}, {"/b", 8}, {"/c", 3}});
        BOOST_TEST(ChooseStorageDir(kDirs, 0, 10, 0, space).value() == 1U);
    }

    BOOST_AUTO_TEST_CASE(HasNothingToChooseWhenNoneCanBeWritten) {
        BOOST_TEST(!ChooseStorageDir(kDirs, 0, 10, 0, freeSpace({})).has_value());
        BOOST_TEST(!ChooseStorageDir({}, 0, 10, 0, freeSpace({{"/a", 100}})).has_value());
    }

    BOOST_AUTO_TEST_CASE(AnObjectWithNoDirectoryIsInTheDataDirectory) {
        BOOST_TEST(ObjectRoot("", "/data/esm") == std::filesystem::path("/data/esm"));
        BOOST_TEST(ObjectRoot("/mnt/disk2/esm", "/data/esm") == std::filesystem::path("/mnt/disk2/esm"));
    }

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(Storing, Fixture)

    BOOST_AUTO_TEST_CASE(ObjectsAreSpreadOverTheDirectoriesAndReadBack) {
        for (const auto *key: {"one.txt", "two.txt", "three.txt", "four.txt"}) BOOST_TEST_REQUIRE(put(key) == 200);

        const auto one = stored("one.txt");
        const auto two = stored("two.txt");
        const auto three = stored("three.txt");
        const auto four = stored("four.txt");

        // In turn: consecutive objects in different directories, and both directories used.
        BOOST_TEST(one.internalDir != two.internalDir);
        BOOST_TEST(two.internalDir != three.internalDir);
        BOOST_TEST(three.internalDir != four.internalDir);
        for (const auto &object: {one, two, three, four}) {
            BOOST_TEST((object.internalDir == volumeA().string() || object.internalDir == volumeB().string()));
            BOOST_TEST(onDisk(object));
            // And not in the data directory, where an object with no directory would be.
            std::error_code ec;
            BOOST_TEST(!std::filesystem::exists(Euclid::Core::DirUtils::FindFilePath(dataDir(), object.internalName), ec));
        }

        const auto [status, body] = get("three.txt");
        BOOST_TEST(status == 200);
        BOOST_TEST(body == kBody);
    }

    BOOST_AUTO_TEST_CASE(ADeleteRemovesTheFileFromItsDirectory) {
        BOOST_TEST_REQUIRE(put("gone.txt") == 200);
        const auto object = stored("gone.txt");
        BOOST_TEST_REQUIRE(onDisk(object));

        BOOST_TEST(call("delete-object", boost::json::serialize(boost::json::object{{"ern", object.ern}})).first == 200);
        BOOST_TEST(!onDisk(object));
    }

    BOOST_AUTO_TEST_CASE(AReuploadRemovesTheOldFileFromItsOwnDirectory) {
        BOOST_TEST_REQUIRE(put("again.txt") == 200);
        const auto first = stored("again.txt");
        BOOST_TEST_REQUIRE(put("again.txt") == 200);
        const auto second = stored("again.txt");

        // The second write went to the other directory; the first file still went from the first.
        BOOST_TEST(first.internalDir != second.internalDir);
        BOOST_TEST(!onDisk(first));
        BOOST_TEST(onDisk(second));
    }

    BOOST_AUTO_TEST_CASE(ACopyIsANewObjectInTheNextDirectory) {
        BOOST_TEST_REQUIRE(put("original.txt") == 200);
        const boost::json::object request{{"sourceBucketErn", bucketErn()}, {"sourceKey", "original.txt"},
                                          {"targetBucketErn", bucketErn()}, {"targetKey", "copy.txt"}};
        BOOST_TEST_REQUIRE(call("copy-object", boost::json::serialize(request)).first == 200);

        const auto original = stored("original.txt");
        const auto copy = stored("copy.txt");
        BOOST_TEST(copy.internalName != original.internalName);
        BOOST_TEST(copy.internalDir != original.internalDir);
        BOOST_TEST(onDisk(original));
        BOOST_TEST(onDisk(copy));
        BOOST_TEST(get("copy.txt").second == kBody);
    }

    BOOST_AUTO_TEST_CASE(AMoveKeepsTheFileWhereItIs) {
        BOOST_TEST_REQUIRE(put("before.txt") == 200);
        const auto before = stored("before.txt");
        const boost::json::object request{{"sourceBucketErn", bucketErn()}, {"sourceKey", "before.txt"},
                                          {"targetBucketErn", bucketErn()}, {"targetKey", "after.txt"}};
        BOOST_TEST_REQUIRE(call("move-object", boost::json::serialize(request)).first == 200);

        const auto after = stored("after.txt");
        BOOST_TEST(after.internalName == before.internalName);
        BOOST_TEST(after.internalDir == before.internalDir);
        BOOST_TEST(get("after.txt").second == kBody);
    }

    BOOST_AUTO_TEST_CASE(WithoutStorageDirsTheDataDirectoryAsBefore) {
        Euclid::Core::Configuration::instance().set<std::vector<std::string> >("euclid.modules.esm.storage-dirs", {});
        BOOST_TEST_REQUIRE(put("plain.txt") == 200);
        const auto object = stored("plain.txt");
        BOOST_TEST(object.internalDir.empty());
        std::error_code ec;
        BOOST_TEST(std::filesystem::exists(Euclid::Core::DirUtils::FindFilePath(dataDir(), object.internalName), ec));
        BOOST_TEST(get("plain.txt").second == kBody);
    }

BOOST_AUTO_TEST_SUITE_END()
