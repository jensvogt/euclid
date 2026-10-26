// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmLifecycleTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <ostream>
#include <random>
#include <string>

// Boost includes
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <EsmServer.h>
#include <euclid/core/ArchiveZip.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DirUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/esm/Lifecycle.h>

// ESM lifecycle management: a bucket's rules, the STANDARD and ARCHIVED states, the archive
// directory an archived object's file moves to, the grace period a restore gives, and the sweep
// that applies the rules. The decision itself first, without a server; then the actions, driven
// through ESM's socket as an administrator.

// For BOOST_TEST to print a state it compares; found by argument-dependent lookup.
namespace Euclid::Database::Entity::ESM {
    inline std::ostream &operator<<(std::ostream &os, const LifecycleState state) { return os << LifecycleStateToString(state); }
}// namespace Euclid::Database::Entity::ESM

namespace {

    using namespace std::chrono_literals;
    using Euclid::Database::Entity::ESM::LifecycleFacts;
    using Euclid::Database::Entity::ESM::LifecycleRule;
    using Euclid::Database::Entity::ESM::LifecycleState;
    using Euclid::Database::Entity::ESM::RuleFor;
    using Euclid::Database::Entity::ESM::ShouldArchive;

    LifecycleRule rule(const std::string &id, const std::string &prefix, const long after, const long restore = 7) {
        LifecycleRule r;
        r.id = id;
        r.prefix = prefix;
        r.archiveAfterDays = after;
        r.restoreDays = restore;
        return r;
    }

}// namespace

BOOST_AUTO_TEST_SUITE(LifecycleDecision)

    BOOST_AUTO_TEST_CASE(TheLongestMatchingEnabledPrefixDecides) {
        auto disabled = rule("off", "logs/2026/", 1);
        disabled.enabled = false;
        const std::vector rules{rule("all", "", 90), rule("logs", "logs/", 14), disabled};

        BOOST_TEST(RuleFor(rules, "a.txt")->id == "all");
        BOOST_TEST(RuleFor(rules, "logs/x.log")->id == "logs");
        BOOST_TEST(RuleFor(rules, "logs/2026/x.log")->id == "logs");
        BOOST_TEST(RuleFor({rule("logs", "logs/", 14)}, "a.txt") == nullptr);
    }

    BOOST_AUTO_TEST_CASE(ARuleNeedsAnIdAndDaysInRange) {
        BOOST_TEST(!Euclid::Database::Entity::ESM::LifecycleRuleProblem(rule("ok", "", 30)).has_value());
        BOOST_TEST(Euclid::Database::Entity::ESM::LifecycleRuleProblem(rule("", "", 30)).has_value());
        BOOST_TEST(Euclid::Database::Entity::ESM::LifecycleRuleProblem(rule("x", "", 0)).has_value());
        BOOST_TEST(Euclid::Database::Entity::ESM::LifecycleRuleProblem(rule("x", "", 30, 0)).has_value());
    }

    BOOST_AUTO_TEST_CASE(UnusedForLongEnoughIsArchived) {
        const auto now = std::chrono::system_clock::now();
        LifecycleFacts facts;
        facts.lastUsed = now - std::chrono::days(31);
        BOOST_TEST(ShouldArchive(facts, rule("r", "", 30), now));

        facts.lastUsed = now - std::chrono::days(29);
        BOOST_TEST(!ShouldArchive(facts, rule("r", "", 30), now));
    }

    BOOST_AUTO_TEST_CASE(OnlyCompleteOnlineObjectsAreArchived) {
        const auto now = std::chrono::system_clock::now();
        LifecycleFacts facts;
        facts.lastUsed = now - std::chrono::days(365);
        facts.state = LifecycleState::ARCHIVED;
        BOOST_TEST(!ShouldArchive(facts, rule("r", "", 30), now));

        facts.state = LifecycleState::STANDARD;
        facts.archivable = false;
        BOOST_TEST(!ShouldArchive(facts, rule("r", "", 30), now));
    }

    BOOST_AUTO_TEST_CASE(ARestoredObjectStaysOnlineForItsGracePeriod) {
        const auto now = std::chrono::system_clock::now();
        LifecycleFacts facts;
        facts.lastUsed = now - std::chrono::days(365);
        facts.restoredAt = now - std::chrono::days(2);
        facts.restoredUntil = now + std::chrono::days(5);
        BOOST_TEST(!ShouldArchive(facts, rule("r", "", 30), now));
    }

    BOOST_AUTO_TEST_CASE(ARestoredObjectNobodyReadGoesBackWhenTheGraceEnds) {
        const auto now = std::chrono::system_clock::now();
        LifecycleFacts facts;
        facts.restoredAt = now - std::chrono::days(8);
        facts.restoredUntil = now - std::chrono::days(1);
        // Recent by its last use - a restore is not one - and archived anyway.
        facts.lastUsed = now - std::chrono::days(8);
        facts.lastAccessed = now - std::chrono::days(400);
        BOOST_TEST(ShouldArchive(facts, rule("r", "", 30), now));
    }

    BOOST_AUTO_TEST_CASE(ARestoredObjectThatWasReadFollowsTheRuleFromThatRead) {
        const auto now = std::chrono::system_clock::now();
        LifecycleFacts facts;
        facts.restoredAt = now - std::chrono::days(8);
        facts.restoredUntil = now - std::chrono::days(1);
        facts.lastAccessed = now - std::chrono::days(6);
        facts.lastUsed = *facts.lastAccessed;
        BOOST_TEST(!ShouldArchive(facts, rule("r", "", 30), now));
        BOOST_TEST(ShouldArchive(facts, rule("r", "", 30), now + std::chrono::days(25)));
    }

BOOST_AUTO_TEST_SUITE_END()

namespace {

    namespace http = boost::beast::http;

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";
    constexpr auto kAccount = "000000000000";
    constexpr auto kRegion = "eu-central-1";
    constexpr auto kNamespace = "development";

    std::filesystem::path scratch() {
        static const auto path = std::filesystem::temp_directory_path() / ("euclid-lifecycle-test-" + std::to_string(std::random_device{}()));
        return path;
    }

    std::filesystem::path dataDir() { return scratch() / "esm"; }
    std::filesystem::path archiveDir() { return scratch() / "esm-archive"; }

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
            configuration.set<std::string>("euclid.modules.esm.archive-dir", archiveDir().string());
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
            factory.esmRepository()->setLifecycleCursor("", "");
            std::error_code ec;
            std::filesystem::remove_all(archiveDir(), ec);
        }
    };

    http::response<http::string_body> send(const std::string &action, const std::string &body, const std::map<std::string, std::string> &headers = {}) {
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
        return res;
    }

    std::pair<int, boost::json::object> ask(const std::string &action, const boost::json::object &body) {
        const auto res = send(action, boost::json::serialize(body));
        // Not every action answers with a body - delete-object answers with none.
        boost::system::error_code ec;
        const auto parsed = boost::json::parse(res.body(), ec);
        return {static_cast<int>(res.result_int()), !ec && parsed.is_object() ? parsed.as_object() : boost::json::object{}};
    }

    int put(const std::string &key, const std::string &data = "hello, world") {
        return static_cast<int>(send("put-object", data, {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", key}}).result_int());
    }

    int getObject(const std::string &key) {
        return static_cast<int>(send("get-object", "", {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", key}, {"x-euclid-part-size", "1048576"}}).result_int());
    }

    Euclid::Database::Entity::ESM::Object stored(const std::string &key) {
        const auto object = Euclid::Database::RepositoryFactory::instance().esmRepository()->findObjectByBucketAndKey(bucketErn(), key);
        BOOST_TEST_REQUIRE(object.has_value());
        return *object;
    }

    // archive-<bucket>-<MMYYYY>.zip, under the account and namespace.
    std::string expectedArchiveFile() {
        const std::chrono::year_month_day day{std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
        char month[8];
        std::snprintf(month, sizeof month, "%02u%04d", static_cast<unsigned>(day.month()), static_cast<int>(day.year()));
        return std::string(kAccount) + "/" + kNamespace + "/archive-incoming-" + month + ".zip";
    }

    bool fileIn(const std::filesystem::path &root, const std::string &internalName) {
        std::error_code ec;
        return std::filesystem::exists(Euclid::Core::DirUtils::FindFilePath(root, internalName), ec);
    }

    int archive(const std::string &key) {
        return ask("archive-object", {{"bucketErn", bucketErn()}, {"key", key}}).first;
    }

    void lastUsedDaysAgo(const std::string &key, const int days) {
        std::ignore = Euclid::Database::RepositoryFactory::instance().esmRepository()->recordObjectAccess(
                stored(key).ern, std::chrono::system_clock::now() - std::chrono::days(days));
    }

    int setRules(const boost::json::array &rules) {
        return ask("set-bucket-lifecycle", {{"ern", bucketErn()}, {"rules", rules}}).first;
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(EsmLifecycleTest, Fixture)

    BOOST_AUTO_TEST_CASE(RulesAreSetReadAndRemoved) {
        BOOST_TEST_REQUIRE(setRules({{{"id", "cold"}, {"archiveAfterDays", 90}, {"restoreDays", 7}},
                                     {{"id", "logs"}, {"prefix", "logs/"}, {"archiveAfterDays", 14}, {"restoreDays", 2}, {"enabled", false}}}) == 200);

        const auto [status, body] = ask("get-bucket-lifecycle", {{"ern", bucketErn()}});
        BOOST_TEST_REQUIRE(status == 200);
        const auto &rules = body.at("rules").as_array();
        BOOST_TEST_REQUIRE(rules.size() == 2u);
        BOOST_TEST(rules[0].at("id").as_string() == "cold");
        BOOST_TEST(rules[0].at("archiveAfterDays").as_int64() == 90);
        BOOST_TEST(rules[1].at("prefix").as_string() == "logs/");
        BOOST_TEST(rules[1].at("enabled").as_bool() == false);

        BOOST_TEST(ask("delete-bucket-lifecycle", {{"ern", bucketErn()}}).first == 200);
        BOOST_TEST(ask("get-bucket-lifecycle", {{"ern", bucketErn()}}).second.at("rules").as_array().empty());
    }

    BOOST_AUTO_TEST_CASE(BadRulesAreRefusedAsAWhole) {
        BOOST_TEST(setRules({{{"id", "ok"}, {"archiveAfterDays", 30}, {"restoreDays", 7}},
                             {{"id", "bad"}, {"archiveAfterDays", 0}, {"restoreDays", 7}}}) == 400);
        BOOST_TEST(setRules({{{"id", "twice"}, {"archiveAfterDays", 30}, {"restoreDays", 7}},
                             {{"id", "twice"}, {"archiveAfterDays", 60}, {"restoreDays", 7}}}) == 400);
        BOOST_TEST(ask("get-bucket-lifecycle", {{"ern", bucketErn()}}).second.at("rules").as_array().empty());
    }

    BOOST_AUTO_TEST_CASE(AnArchivedObjectsFileMovesAndReadingItIsRefused) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        const auto internalName = stored("a.txt").internalName;
        BOOST_TEST_REQUIRE(archive("a.txt") == 200);

        BOOST_TEST(stored("a.txt").lifecycleState == LifecycleState::ARCHIVED);
        BOOST_TEST(stored("a.txt").archivedAt.has_value());
        BOOST_TEST(!fileIn(dataDir(), internalName));

        // Into this month's zip for the bucket, under the object's key.
        const auto object = stored("a.txt");
        BOOST_TEST(object.archiveFile == expectedArchiveFile());
        const auto entries = Euclid::Core::ArchiveZip::List(archiveDir() / object.archiveFile);
        BOOST_TEST_REQUIRE(entries.size() == 1u);
        BOOST_TEST(entries[0].name == "a.txt");
        BOOST_TEST(entries[0].member.offset == object.archiveMember.offset);
        BOOST_TEST(entries[0].member.size == 12u);

        BOOST_TEST(getObject("a.txt") == 409);
        BOOST_TEST(ask("create-download", {{"bucketErn", bucketErn()}, {"key", "a.txt"}}).first == 409);
        BOOST_TEST(ask("copy-object", {{"sourceBucketErn", bucketErn()}, {"sourceKey", "a.txt"},
                                       {"targetBucketErn", bucketErn()}, {"targetKey", "b.txt"}}).first == 409);
        // What it is, as opposed to what is in it, is still there to ask.
        BOOST_TEST(ask("get-object-size", {{"bucketErn", bucketErn()}, {"key", "a.txt"}}).first == 200);
        // And asking again changes nothing.
        BOOST_TEST(archive("a.txt") == 200);
    }

    BOOST_AUTO_TEST_CASE(ABucketsObjectsShareTheMonthsZip) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        BOOST_TEST_REQUIRE(put("dir/b.txt", "other bytes") == 200);
        BOOST_TEST_REQUIRE(archive("a.txt") == 200);
        BOOST_TEST_REQUIRE(archive("dir/b.txt") == 200);

        BOOST_TEST(stored("dir/b.txt").archiveFile == stored("a.txt").archiveFile);
        const auto entries = Euclid::Core::ArchiveZip::List(archiveDir() / expectedArchiveFile());
        BOOST_TEST_REQUIRE(entries.size() == 2u);
        BOOST_TEST(entries[1].name == "dir/b.txt");
    }

    BOOST_AUTO_TEST_CASE(ARestoreBringsItBackForTheGracePeriod) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        const auto internalName = stored("a.txt").internalName;
        BOOST_TEST_REQUIRE(archive("a.txt") == 200);

        const auto before = std::chrono::system_clock::now();
        const auto [status, body] = ask("restore-object", {{"bucketErn", bucketErn()}, {"key", "a.txt"}, {"days", 3}});
        BOOST_TEST_REQUIRE(status == 200);
        BOOST_TEST(body.at("lifecycleState").as_string() == "STANDARD");

        const auto object = stored("a.txt");
        BOOST_TEST(object.lifecycleState == LifecycleState::STANDARD);
        BOOST_TEST_REQUIRE(object.restoredUntil.has_value());
        BOOST_TEST((*object.restoredUntil >= before + std::chrono::days(3) - 1min));
        BOOST_TEST((*object.restoredUntil <= std::chrono::system_clock::now() + std::chrono::days(3)));
        BOOST_TEST(object.archiveFile.empty());
        BOOST_TEST(fileIn(dataDir(), internalName));
        const auto res = send("get-object", "", {{"x-euclid-bucket-ern", bucketErn()}, {"x-euclid-key", "a.txt"}, {"x-euclid-part-size", "1048576"}});
        BOOST_TEST(res.result_int() == 200u);
        BOOST_TEST(res.body() == "hello, world");
    }

    BOOST_AUTO_TEST_CASE(ARestoreWithoutDaysTakesTheRulesGracePeriod) {
        BOOST_TEST_REQUIRE(setRules({{{"id", "r"}, {"archiveAfterDays", 30}, {"restoreDays", 11}}}) == 200);
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        BOOST_TEST_REQUIRE(archive("a.txt") == 200);
        BOOST_TEST_REQUIRE(ask("restore-object", {{"bucketErn", bucketErn()}, {"key", "a.txt"}}).first == 200);

        const auto until = *stored("a.txt").restoredUntil;
        BOOST_TEST((until > std::chrono::system_clock::now() + std::chrono::days(10)));
        BOOST_TEST((until <= std::chrono::system_clock::now() + std::chrono::days(11)));
    }

    BOOST_AUTO_TEST_CASE(AZipNothingIsArchivedInAnyMoreIsRemovedOnceSettled) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        BOOST_TEST_REQUIRE(put("b.txt") == 200);
        BOOST_TEST_REQUIRE(archive("a.txt") == 200);
        BOOST_TEST_REQUIRE(archive("b.txt") == 200);
        const auto zip = archiveDir() / expectedArchiveFile();

        BOOST_TEST_REQUIRE(ask("delete-object", {{"ern", stored("a.txt").ern}}).first == 200);
        std::filesystem::last_write_time(zip, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
        std::ignore = Euclid::ESM::EsmServer::SweepLifecycle(60s);
        // b.txt is still in it.
        BOOST_TEST(std::filesystem::exists(zip));

        BOOST_TEST_REQUIRE(ask("restore-object", {{"bucketErn", bucketErn()}, {"key", "b.txt"}}).first == 200);
        // Just written to is not settled - but a restore does not write to it.
        std::filesystem::last_write_time(zip, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
        std::ignore = Euclid::ESM::EsmServer::SweepLifecycle(60s);
        BOOST_TEST(!std::filesystem::exists(zip));
        BOOST_TEST(getObject("b.txt") == 200);
    }

    BOOST_AUTO_TEST_CASE(ARecentlyWrittenZipIsKept) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        BOOST_TEST_REQUIRE(archive("a.txt") == 200);
        BOOST_TEST_REQUIRE(ask("delete-object", {{"ern", stored("a.txt").ern}}).first == 200);
        std::ignore = Euclid::ESM::EsmServer::SweepLifecycle(60s);
        BOOST_TEST(std::filesystem::exists(archiveDir() / expectedArchiveFile()));
    }

    BOOST_AUTO_TEST_CASE(WritingOverAnArchivedObjectPutsItOnline) {
        BOOST_TEST_REQUIRE(put("a.txt") == 200);
        const auto old = stored("a.txt").internalName;
        BOOST_TEST_REQUIRE(archive("a.txt") == 200);

        BOOST_TEST_REQUIRE(put("a.txt", "new bytes") == 200);
        const auto object = stored("a.txt");
        BOOST_TEST(object.lifecycleState == LifecycleState::STANDARD);
        BOOST_TEST(!object.archivedAt.has_value());
        BOOST_TEST(object.archiveFile.empty());
        BOOST_TEST(!fileIn(dataDir(), old));
        BOOST_TEST(getObject("a.txt") == 200);
    }

    BOOST_AUTO_TEST_CASE(TheSweepArchivesWhatTheRulesSay) {
        BOOST_TEST_REQUIRE(setRules({{{"id", "logs"}, {"prefix", "logs/"}, {"archiveAfterDays", 30}, {"restoreDays", 7}}}) == 200);
        for (const auto *key: {"logs/old.log", "logs/new.log", "data/old.bin"}) BOOST_TEST_REQUIRE(put(key) == 200);
        lastUsedDaysAgo("logs/old.log", 40);
        lastUsedDaysAgo("logs/new.log", 5);
        lastUsedDaysAgo("data/old.bin", 400);

        BOOST_TEST(Euclid::ESM::EsmServer::SweepLifecycle(60s) == 1);
        BOOST_TEST(stored("logs/old.log").lifecycleState == LifecycleState::ARCHIVED);
        BOOST_TEST(stored("logs/new.log").lifecycleState == LifecycleState::STANDARD);
        BOOST_TEST(stored("data/old.bin").lifecycleState == LifecycleState::STANDARD);
        // A finished pass leaves no cursor behind.
        BOOST_TEST(!Euclid::Database::RepositoryFactory::instance().esmRepository()->lifecycleCursor().has_value());
    }

    BOOST_AUTO_TEST_CASE(TheSweepWalksABucketInPages) {
        Euclid::Core::Configuration::instance().set<long>("euclid.modules.esm.lifecycle-page-size", 2);
        BOOST_TEST_REQUIRE(setRules({{{"id", "all"}, {"archiveAfterDays", 30}, {"restoreDays", 7}}}) == 200);
        for (int i = 0; i < 5; ++i) {
            const auto key = "k" + std::to_string(i);
            BOOST_TEST_REQUIRE(put(key) == 200);
            lastUsedDaysAgo(key, 31);
        }
        BOOST_TEST(Euclid::ESM::EsmServer::SweepLifecycle(60s) == 5);
        Euclid::Core::Configuration::instance().set<long>("euclid.modules.esm.lifecycle-page-size", 1000);
    }

    BOOST_AUTO_TEST_CASE(TheSweepTakesBackARestoreNobodyUsed) {
        BOOST_TEST_REQUIRE(setRules({{{"id", "all"}, {"archiveAfterDays", 30}, {"restoreDays", 7}}}) == 200);
        BOOST_TEST_REQUIRE(put("unread.txt") == 200);
        BOOST_TEST_REQUIRE(put("read.txt") == 200);
        for (const auto *key: {"unread.txt", "read.txt"}) {
            BOOST_TEST_REQUIRE(archive(key) == 200);
            BOOST_TEST_REQUIRE(ask("restore-object", {{"bucketErn", bucketErn()}, {"key", key}}).first == 200);
        }

        // Both graces over: restored eight days ago until yesterday; one read since.
        const auto repo = Euclid::Database::RepositoryFactory::instance().esmRepository();
        const auto now = std::chrono::system_clock::now();
        for (const auto *key: {"unread.txt", "read.txt"}) {
            auto object = stored(key);
            object.restoredAt = now - std::chrono::days(8);
            object.restoredUntil = now - std::chrono::days(1);
            BOOST_TEST_REQUIRE(repo->setObjectLifecycle(object, LifecycleState::STANDARD));
        }
        std::ignore = repo->recordObjectAccess(stored("unread.txt").ern, now - std::chrono::days(9));
        std::ignore = repo->recordObjectAccess(stored("read.txt").ern, now - std::chrono::days(2));

        BOOST_TEST(Euclid::ESM::EsmServer::SweepLifecycle(60s) == 1);
        BOOST_TEST(stored("unread.txt").lifecycleState == LifecycleState::ARCHIVED);
        BOOST_TEST(stored("read.txt").lifecycleState == LifecycleState::STANDARD);
    }

BOOST_AUTO_TEST_SUITE_END()
