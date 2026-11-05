// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmVersioningTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>

// Boost includes
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <EsmServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/esm/Versioning.h>

// Bucket versioning: with it on, an upload over an existing key keeps the object it replaces when
// the bytes differ and is ignored when they are the same; without it, uploads replace as they
// always have. Driven through ESM's socket as an administrator, against the in-memory store, with
// the object files checked on disk - a version that is listed but whose file was removed, or one
// deleted whose file was left behind, are the two ways this can go wrong without any request
// failing.

namespace {

    namespace http = boost::beast::http;
    using Euclid::Database::Entity::ESM::DecideUploadOutcome;
    using Euclid::Database::Entity::ESM::Object;
    using Euclid::Database::Entity::ESM::ObjectStatus;
    using Euclid::Database::Entity::ESM::UploadOutcome;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-versioning-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    std::string bucketErn(const std::string &name) { return Euclid::Core::createEsmBucketErn(kAccount, kNamespace, name); }

    std::string objectErn(const std::string &bucket, const std::string &key) {
        return Euclid::Core::createEsmObjectErn(kAccount, kNamespace, bucket + "/" + key);
    }

    Euclid::ESM::EsmServer &server() {
        static Euclid::ESM::EsmServer *instance = [] {
            std::filesystem::create_directories(scratch() / "esm");
            auto *created = new Euclid::ESM::EsmServer((scratch() / "esm.sock").string(), 2);
            created->start();
            return created;
        }();
        return *instance;
    }

    void addBucket(const std::string &name, const bool versioning) {
        Euclid::Database::Entity::ESM::Bucket bucket;
        bucket.name = name;
        bucket.ern = bucketErn(name);
        bucket.accountId = kAccount;
        bucket.nameSpace = kNamespace;
        bucket.region = kRegion;
        bucket.versioning = versioning;
        std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->upsertBucket(bucket);
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

            addBucket("plain", false);
            addBucket("versioned", true);
        }
    };

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

    Answer put(const std::string &bucket, const std::string &key, const std::string &data) {
        return send("put-object", data, {{"x-euclid-bucket-ern", bucketErn(bucket)}, {"x-euclid-key", key}});
    }

    Answer get(const std::string &bucket, const std::string &key, const std::string &versionId = {}) {
        std::map<std::string, std::string> headers{{"x-euclid-bucket-ern", bucketErn(bucket)}, {"x-euclid-key", key}, {"x-euclid-part-size", "1048576"}};
        if (!versionId.empty()) headers["x-euclid-version-id"] = versionId;
        return send("get-object", "", headers);
    }

    std::optional<Object> current(const std::string &bucket, const std::string &key) {
        return Euclid::Database::RepositoryFactory::instance().esmRepository()->findObjectByBucketAndKey(bucketErn(bucket), key);
    }

    std::vector<Object> versions(const std::string &bucket, const std::string &key) {
        return Euclid::Database::RepositoryFactory::instance().esmRepository()->listObjectVersions(objectErn(bucket, key));
    }

    // Whether a file of this internal name is anywhere under the data directory - the fan-out puts
    // it two levels down, and which storage directory it went to is not this test's business.
    bool onDisk(const std::string &internalName) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(scratch() / "esm", ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file() && it->path().filename() == internalName) return true;
        }
        return false;
    }

    std::string md5(const std::string &data) { return Euclid::Core::CryptoUtils::md5Sum(data); }

}// namespace

// ── The decision itself ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(WithoutVersioningEveryUploadReplaces) {
    Object existing;
    existing.status = ObjectStatus::COMPLETED;
    existing.md5Sum = md5("a");

    BOOST_TEST((DecideUploadOutcome(false, existing, md5("a")) == UploadOutcome::REPLACE));
    BOOST_TEST((DecideUploadOutcome(false, existing, md5("b")) == UploadOutcome::REPLACE));
    BOOST_TEST((DecideUploadOutcome(false, std::nullopt, md5("a")) == UploadOutcome::REPLACE));
}

BOOST_AUTO_TEST_CASE(WithVersioningTheMd5Decides) {
    Object existing;
    existing.status = ObjectStatus::COMPLETED;
    existing.md5Sum = md5("a");

    BOOST_TEST((DecideUploadOutcome(true, existing, md5("a")) == UploadOutcome::UNCHANGED));
    BOOST_TEST((DecideUploadOutcome(true, existing, md5("b")) == UploadOutcome::NEW_VERSION));
    // A first upload has nothing to keep.
    BOOST_TEST((DecideUploadOutcome(true, std::nullopt, md5("a")) == UploadOutcome::REPLACE));
}

BOOST_AUTO_TEST_CASE(AnUnfinishedObjectIsNotAVersion) {
    // A key whose first upload has not completed has no earlier bytes to preserve or compare.
    Object existing;
    existing.status = ObjectStatus::UPLOADING;
    existing.md5Sum = md5("a");
    BOOST_TEST((DecideUploadOutcome(true, existing, md5("a")) == UploadOutcome::REPLACE));

    existing.status = ObjectStatus::COMPLETED;
    existing.md5Sum.clear();
    BOOST_TEST((DecideUploadOutcome(true, existing, md5("a")) == UploadOutcome::REPLACE));
}

BOOST_AUTO_TEST_CASE(ASupersededObjectGetsAVersionIdIfItHadNone) {
    Object stored;
    stored.ern = "ern:esm:x";
    stored.oid = "abc";
    const auto kept = Euclid::Database::Entity::ESM::SupersededVersion(stored);
    BOOST_TEST(!kept.versionId.empty());
    BOOST_TEST(kept.oid.empty());

    stored.versionId = "v1";
    BOOST_TEST(Euclid::Database::Entity::ESM::SupersededVersion(stored).versionId == "v1");
}

// ── put-object ──────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(WithoutVersioningAnUploadReplacesTheObjectAndItsFile, Fixture) {
    BOOST_REQUIRE(put("plain", "a.txt", "first").status == 200);
    const auto first = current("plain", "a.txt");
    BOOST_REQUIRE(first.has_value());

    const auto second = put("plain", "a.txt", "second");
    BOOST_REQUIRE(second.status == 200);
    BOOST_TEST(!second.json().at("unchanged").as_bool());

    BOOST_TEST(current("plain", "a.txt")->md5Sum == md5("second"));
    BOOST_TEST(versions("plain", "a.txt").empty());
    BOOST_TEST(!onDisk(first->internalName));
}

BOOST_FIXTURE_TEST_CASE(WithoutVersioningTheSameBytesAreStoredAgain, Fixture) {
    // The behaviour unversioned buckets have always had: identical bytes still replace.
    BOOST_REQUIRE(put("plain", "same.txt", "bytes").status == 200);
    const auto first = current("plain", "same.txt");
    const auto again = put("plain", "same.txt", "bytes");
    BOOST_TEST(!again.json().at("unchanged").as_bool());
    BOOST_TEST(current("plain", "same.txt")->internalName != first->internalName);
}

BOOST_FIXTURE_TEST_CASE(TheSameBytesAreIgnored, Fixture) {
    BOOST_REQUIRE(put("versioned", "same.txt", "bytes").status == 200);
    const auto first = current("versioned", "same.txt");
    BOOST_REQUIRE(first.has_value());
    BOOST_TEST(!first->versionId.empty());

    const auto again = put("versioned", "same.txt", "bytes");
    BOOST_REQUIRE(again.status == 200);
    const auto answer = again.json();
    BOOST_TEST(answer.at("unchanged").as_bool());
    BOOST_TEST(std::string(answer.at("versionId").as_string()) == first->versionId);

    // Nothing written: the same row, the same file, no version, no second file.
    const auto after = current("versioned", "same.txt");
    BOOST_TEST(after->internalName == first->internalName);
    BOOST_TEST(after->versionId == first->versionId);
    BOOST_TEST(versions("versioned", "same.txt").empty());
    BOOST_TEST(onDisk(first->internalName));
}

BOOST_FIXTURE_TEST_CASE(DifferentBytesMakeANewVersionAndKeepTheOldOne, Fixture) {
    BOOST_REQUIRE(put("versioned", "doc.txt", "version one").status == 200);
    const auto first = current("versioned", "doc.txt");

    const auto second = put("versioned", "doc.txt", "version two");
    BOOST_REQUIRE(second.status == 200);
    BOOST_TEST(!second.json().at("unchanged").as_bool());

    const auto now = current("versioned", "doc.txt");
    BOOST_TEST(now->md5Sum == md5("version two"));
    BOOST_TEST(now->versionId != first->versionId);

    const auto kept = versions("versioned", "doc.txt");
    BOOST_REQUIRE(kept.size() == 1u);
    BOOST_TEST(kept.front().versionId == first->versionId);
    BOOST_TEST(kept.front().md5Sum == md5("version one"));
    BOOST_TEST(kept.front().internalName == first->internalName);
    BOOST_TEST(onDisk(first->internalName));
    BOOST_TEST(onDisk(now->internalName));
}

BOOST_FIXTURE_TEST_CASE(TheCountersDescribeTheCurrentObjectOnly, Fixture) {
    const auto repo = Euclid::Database::RepositoryFactory::instance().esmRepository();
    const auto before = repo->findBucketByErn(bucketErn("versioned"));

    BOOST_REQUIRE(put("versioned", "count.txt", "12345").status == 200);
    BOOST_REQUIRE(put("versioned", "count.txt", "1234567890").status == 200);

    const auto after = repo->findBucketByErn(bucketErn("versioned"));
    BOOST_TEST(after->objects - before->objects == 1);
    BOOST_TEST(after->size - before->size == 10);
}

// ── Reading and listing versions ────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(AnOldVersionCanBeRead, Fixture) {
    BOOST_REQUIRE(put("versioned", "read.txt", "old bytes").status == 200);
    const auto old = current("versioned", "read.txt");
    BOOST_REQUIRE(put("versioned", "read.txt", "new bytes").status == 200);

    BOOST_TEST(get("versioned", "read.txt").body == "new bytes");
    const auto previous = get("versioned", "read.txt", old->versionId);
    BOOST_TEST(previous.status == 200);
    BOOST_TEST(previous.body == "old bytes");
    BOOST_TEST(get("versioned", "read.txt", "no-such-version").status == 404);
}

BOOST_FIXTURE_TEST_CASE(VersionsAreListedCurrentFirstThenNewestFirst, Fixture) {
    BOOST_REQUIRE(put("versioned", "list.txt", "1").status == 200);
    const auto v1 = current("versioned", "list.txt")->versionId;
    BOOST_REQUIRE(put("versioned", "list.txt", "2").status == 200);
    const auto v2 = current("versioned", "list.txt")->versionId;
    BOOST_REQUIRE(put("versioned", "list.txt", "3").status == 200);
    const auto v3 = current("versioned", "list.txt")->versionId;

    const auto listed = ask("list-object-versions", {{"bucketErn", bucketErn("versioned")}, {"key", "list.txt"}});
    BOOST_REQUIRE(listed.status == 200);
    const auto answer = listed.json();
    const auto &list = answer.at("versions").as_array();
    BOOST_REQUIRE(list.size() == 3u);
    BOOST_TEST(std::string(list.at(0).at("versionId").as_string()) == v3);
    BOOST_TEST(list.at(0).at("latest").as_bool());
    BOOST_TEST(std::string(list.at(1).at("versionId").as_string()) == v2);
    BOOST_TEST(std::string(list.at(2).at("versionId").as_string()) == v1);
    BOOST_TEST(!list.at(2).at("latest").as_bool());
}

// ── Deleting ────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(DeletingANonCurrentVersionLeavesTheObjectAlone, Fixture) {
    BOOST_REQUIRE(put("versioned", "del1.txt", "old").status == 200);
    const auto old = current("versioned", "del1.txt");
    BOOST_REQUIRE(put("versioned", "del1.txt", "new").status == 200);
    const auto now = current("versioned", "del1.txt");

    const auto deleted = ask("delete-object", {{"ern", now->ern}, {"versionId", old->versionId}});
    BOOST_REQUIRE(deleted.status == 200);

    BOOST_TEST(versions("versioned", "del1.txt").empty());
    BOOST_TEST(!onDisk(old->internalName));
    BOOST_TEST(current("versioned", "del1.txt")->internalName == now->internalName);
    BOOST_TEST(get("versioned", "del1.txt").body == "new");
}

BOOST_FIXTURE_TEST_CASE(DeletingTheCurrentVersionPromotesTheNewestLeft, Fixture) {
    BOOST_REQUIRE(put("versioned", "del2.txt", "one").status == 200);
    BOOST_REQUIRE(put("versioned", "del2.txt", "two").status == 200);
    const auto two = current("versioned", "del2.txt");
    BOOST_REQUIRE(put("versioned", "del2.txt", "three").status == 200);
    const auto three = current("versioned", "del2.txt");

    const auto deleted = ask("delete-object", {{"ern", three->ern}, {"versionId", three->versionId}});
    BOOST_REQUIRE(deleted.status == 200);
    BOOST_TEST(std::string(deleted.json().at("currentVersionId").as_string()) == two->versionId);

    const auto promoted = current("versioned", "del2.txt");
    BOOST_TEST(promoted->versionId == two->versionId);
    BOOST_TEST(promoted->internalName == two->internalName);
    BOOST_TEST(get("versioned", "del2.txt").body == "two");
    BOOST_TEST(versions("versioned", "del2.txt").size() == 1u);
    BOOST_TEST(!onDisk(three->internalName));
}

BOOST_FIXTURE_TEST_CASE(DeletingTheObjectTakesEveryVersionWithIt, Fixture) {
    BOOST_REQUIRE(put("versioned", "del3.txt", "a").status == 200);
    const auto a = current("versioned", "del3.txt");
    BOOST_REQUIRE(put("versioned", "del3.txt", "b").status == 200);
    const auto b = current("versioned", "del3.txt");

    BOOST_REQUIRE(ask("delete-object", {{"ern", b->ern}}).status == 200);

    BOOST_TEST(!current("versioned", "del3.txt").has_value());
    BOOST_TEST(versions("versioned", "del3.txt").empty());
    BOOST_TEST(!onDisk(a->internalName));
    BOOST_TEST(!onDisk(b->internalName));
}

BOOST_FIXTURE_TEST_CASE(PurgingABucketTakesTheVersionsToo, Fixture) {
    addBucket("purged", true);
    BOOST_REQUIRE(put("purged", "p.txt", "a").status == 200);
    const auto a = current("purged", "p.txt");
    BOOST_REQUIRE(put("purged", "p.txt", "b").status == 200);

    BOOST_REQUIRE(ask("purge-bucket", {{"ern", bucketErn("purged")}}).status == 200);

    BOOST_TEST(versions("purged", "p.txt").empty());
    BOOST_TEST(!onDisk(a->internalName));
}

// ── Moving ──────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(AMovedObjectTakesItsVersionsAlong, Fixture) {
    BOOST_REQUIRE(put("versioned", "from.txt", "a").status == 200);
    BOOST_REQUIRE(put("versioned", "from.txt", "b").status == 200);

    const auto moved = ask("move-object", {{"sourceBucketErn", bucketErn("versioned")}, {"sourceKey", "from.txt"},
                                           {"targetBucketErn", bucketErn("versioned")}, {"targetKey", "to.txt"}});
    BOOST_REQUIRE(moved.status == 200);

    BOOST_TEST(versions("versioned", "from.txt").empty());
    const auto kept = versions("versioned", "to.txt");
    BOOST_REQUIRE(kept.size() == 1u);
    BOOST_TEST(kept.front().key == "to.txt");
    BOOST_TEST(kept.front().md5Sum == md5("a"));
}

// ── Turning it on and off ───────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(VersioningIsSetExplicitly, Fixture) {
    addBucket("toggled", false);
    BOOST_TEST(ask("set-bucket-versioning", {{"ern", bucketErn("toggled")}}).status == 400);

    const auto on = ask("set-bucket-versioning", {{"ern", bucketErn("toggled")}, {"enabled", true}});
    BOOST_REQUIRE(on.status == 200);
    BOOST_TEST(on.json().at("versioning").as_bool());

    BOOST_REQUIRE(put("toggled", "t.txt", "a").status == 200);
    BOOST_REQUIRE(put("toggled", "t.txt", "b").status == 200);
    BOOST_TEST(versions("toggled", "t.txt").size() == 1u);

    // Off again: uploads replace, and the version already kept stays.
    BOOST_REQUIRE(ask("set-bucket-versioning", {{"ern", bucketErn("toggled")}, {"enabled", false}}).status == 200);
    BOOST_REQUIRE(put("toggled", "t.txt", "c").status == 200);
    BOOST_TEST(versions("toggled", "t.txt").size() == 1u);
    BOOST_TEST(current("toggled", "t.txt")->md5Sum == md5("c"));
}

// ── Multipart ───────────────────────────────────────────────────────────────

namespace {

    // A multipart upload of one part, waited for: complete-upload answers before the background
    // pass assembles the file, and the outcome is only decided there.
    void multipart(const std::string &bucket, const std::string &key, const std::string &data) {
        const auto created = ask("create-upload", {{"bucketErn", bucketErn(bucket)}, {"key", key}});
        BOOST_REQUIRE(created.status == 200);
        const auto uploadId = std::string(created.json().at("uploadId").as_string());
        BOOST_REQUIRE(send("upload-part", data, {{"x-euclid-upload-id", uploadId}, {"x-euclid-part-number", "1"}}).status == 200);
        BOOST_REQUIRE(ask("complete-upload", {{"uploadId", uploadId}}).status == 200);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto object = current(bucket, key); object.has_value() && object->status == ObjectStatus::COMPLETED &&
                                                         !std::filesystem::exists(scratch() / "esm" / "uploads" / uploadId)) {
                // The row says COMPLETED both before the upload and after; wait for the scratch
                // storage to go, which happens in every outcome once the pass has decided.
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

}// namespace

BOOST_FIXTURE_TEST_CASE(AMultipartUploadOfTheSameBytesIsIgnored, Fixture) {
    BOOST_REQUIRE(put("versioned", "mp.bin", "multipart bytes").status == 200);
    const auto first = current("versioned", "mp.bin");

    multipart("versioned", "mp.bin", "multipart bytes");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto after = current("versioned", "mp.bin");
    BOOST_TEST((after->status == ObjectStatus::COMPLETED));
    BOOST_TEST(after->internalName == first->internalName);
    BOOST_TEST(versions("versioned", "mp.bin").empty());
}

BOOST_FIXTURE_TEST_CASE(AMultipartUploadOfNewBytesMakesAVersion, Fixture) {
    BOOST_REQUIRE(put("versioned", "mp2.bin", "first bytes").status == 200);
    const auto first = current("versioned", "mp2.bin");

    multipart("versioned", "mp2.bin", "second bytes");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && current("versioned", "mp2.bin")->md5Sum != md5("second bytes")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    BOOST_TEST(current("versioned", "mp2.bin")->md5Sum == md5("second bytes"));
    const auto kept = versions("versioned", "mp2.bin");
    BOOST_REQUIRE(kept.size() == 1u);
    BOOST_TEST(kept.front().internalName == first->internalName);
    BOOST_TEST((kept.front().status == ObjectStatus::COMPLETED));
    BOOST_TEST(onDisk(first->internalName));
}
