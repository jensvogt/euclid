// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE InfrastructureDeclarationTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eap/Infrastructure.h>
#include <euclid/database/entity/eap/InfrastructureApply.h>

using namespace Euclid::Database::Entity::EAP::Infrastructure;

// The declaration an application ships beside its artifact. Everything here is pure: what the file
// may say, what it may not, and what each access level turns into. The reading is where a mistake is
// cheap to make and expensive to find - a file that parsed but granted the wrong thing would be a
// permission nobody asked for, discovered by whatever it let through.

namespace {

    boost::json::value parse(const std::string &json) { return boost::json::parse(json); }

    bool grants(const std::string &kind, const std::string &access, const std::string &permission) {
        const auto permissions = PermissionsFor(kind, access);
        return std::ranges::find(permissions, permission) != permissions.end();
    }

}// namespace

BOOST_AUTO_TEST_SUITE(InfrastructureDeclarationTest)

    // ── where it lives ──────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(TheKeyIsDerivedFromTheApplicationId) {

        // Nothing has to be told where to look: the id is what euclid already holds wherever this is
        // read, so the sidecar is findable from the application record alone.
        BOOST_TEST(ObjectKey("file-copy") == "file-copy.euclid.json");
    }

    // ── what a file may say ─────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(ReadsWhatAnApplicationOwns) {

        const auto read = Read(parse(R"({
            "version": 1,
            "creates": {
                "topics": [{"name": "protokollierung-topic"}],
                "queues": [{"name": "file-logging-cancel-queue"}]
            }
        })"));

        BOOST_TEST(read.error.empty());
        BOOST_TEST(read.declaration.creates.size() == 2U);
        BOOST_TEST(read.declaration.uses.empty());
    }

    BOOST_AUTO_TEST_CASE(ReadsWhatItOnlyReaches) {

        const auto read = Read(parse(R"({
            "version": 1,
            "uses": {
                "buckets": [{"name": "transfer-server", "access": ["subscribe", "read"], "owner": "ftp-server"}],
                "topics": [{"name": "protokollierung-topic", "access": "produce", "owner": "protokollierung-service"}]
            }
        })"));

        BOOST_REQUIRE(read.error.empty());
        BOOST_REQUIRE(read.declaration.uses.size() == 2U);

        // Both spellings of access, because both are already written by hand: one bucket wants two
        // levels as an array and one topic wants one as a string. Refusing either would be refusing a
        // file somebody wrote correctly.
        const auto &bucket = read.declaration.uses[0];
        BOOST_TEST(bucket.kind == "buckets");
        BOOST_TEST(bucket.access.size() == 2U);
        BOOST_TEST(bucket.owner == "ftp-server");

        const auto &topic = read.declaration.uses[1];
        BOOST_TEST(topic.access.size() == 1U);
        BOOST_TEST(topic.access[0] == "produce");
    }

    // ── what it may not ─────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(AVersionIsRequiredRatherThanAssumed) {

        // A file with no version was written against a schema nobody has settled, and reading it as
        // version 1 would apply a guess to somebody's installation.
        BOOST_TEST(!Read(parse(R"({"creates": {"queues": [{"name": "orders"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version": 2, "creates": {}})")).error.empty());
    }

    BOOST_AUTO_TEST_CASE(AnUnknownResourceKindIsRefused) {

        // Not ignored. A kind euclid does not know is a section that would silently do nothing, and
        // the author would be left believing they had declared something.
        const auto read = Read(parse(R"({"version": 1, "creates": {"streams": [{"name": "t"}]}})"));
        BOOST_TEST(!read.error.empty());
        BOOST_TEST(read.error.find("streams") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(AnAccessLevelThatDoesNotExistForThatKindIsRefused) {

        // And says what would have worked, because the answer differs per kind: a queue is consumed
        // from and a topic is subscribed to, and getting that wrong is the likeliest mistake here.
        const auto read = Read(parse(
                R"({"version": 1, "uses": {"topics": [{"name": "t", "access": "consume"}]}})"));
        BOOST_REQUIRE(!read.error.empty());
        BOOST_TEST(read.error.find("produce") != std::string::npos);
        BOOST_TEST(read.error.find("subscribe") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(AUsesEntryWithNoAccessIsRefused) {

        // "I reach this" without saying how is a grant euclid would have to invent.
        BOOST_TEST(!Read(parse(R"({"version": 1, "uses": {"queues": [{"name": "orders"}]}})")).error.empty());
    }

    BOOST_AUTO_TEST_CASE(AnAccessLevelOnSomethingYouOwnIsRefused) {

        // Meaningless rather than wrong - and ignoring it would leave somebody believing they had
        // narrowed their own access to their own queue.
        const auto read = Read(parse(
                R"({"version": 1, "creates": {"queues": [{"name": "orders", "access": "produce"}]}})"));
        BOOST_TEST(!read.error.empty());
    }

    BOOST_AUTO_TEST_CASE(AnEntryWithNoNameIsRefused) {
        BOOST_TEST(!Read(parse(R"({"version": 1, "creates": {"queues": [{}]}})")).error.empty());
    }

    // ── the access table ────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(EveryAccessLevelCanResolveTheNameItWasGrantedOn) {

        // A name is not an ERN and every other action takes the ERN, so an access level without the
        // resolver would be granted on nothing at all.
        BOOST_TEST(grants("queues", "produce", "eqs:get-queue-ern"));
        BOOST_TEST(grants("queues", "consume", "eqs:get-queue-ern"));
        BOOST_TEST(grants("topics", "produce", "ens:get-topic-ern"));
        BOOST_TEST(grants("topics", "subscribe", "ens:get-topic-ern"));
        BOOST_TEST(grants("buckets", "read", "esm:get-bucket-ern"));
        BOOST_TEST(grants("buckets", "write", "esm:get-bucket-ern"));
        BOOST_TEST(grants("buckets", "subscribe", "esm:get-bucket-ern"));
    }

    BOOST_AUTO_TEST_CASE(ProducingAndConsumingAreDifferentGrants) {

        // The service that fills a queue and the service that drains it are usually different, and
        // each reaching only its own end is the whole point of naming a level.
        BOOST_TEST(grants("queues", "produce", "eqs:send-message"));
        BOOST_TEST(!grants("queues", "produce", "eqs:receive-messages"));

        BOOST_TEST(grants("queues", "consume", "eqs:receive-messages"));
        BOOST_TEST(!grants("queues", "consume", "eqs:send-message"));
    }

    BOOST_AUTO_TEST_CASE(WritingABucketCarriesTheMultipartActions) {

        // An object over the part size is uploaded no other way, so a writer without these fails on
        // exactly the files worth storing.
        BOOST_TEST(grants("buckets", "write", "esm:put-object"));
        BOOST_TEST(grants("buckets", "write", "esm:create-upload"));
        BOOST_TEST(grants("buckets", "write", "esm:upload-part"));
        BOOST_TEST(grants("buckets", "write", "esm:complete-upload"));

        // And writing is not reading: a service that only deposits files has no business listing
        // what else is in the bucket.
        BOOST_TEST(!grants("buckets", "write", "esm:get-object"));
        BOOST_TEST(!grants("buckets", "write", "esm:list-objects"));
    }

    BOOST_AUTO_TEST_CASE(ReadingABucketCarriesTheMultipartActions) {

        // An object over the inline limit is answered 413 by get-object and downloaded no other
        // way, so a reader without these fails on exactly the files worth reading.
        BOOST_TEST(grants("buckets", "read", "esm:get-object"));
        BOOST_TEST(grants("buckets", "read", "esm:create-download"));
        BOOST_TEST(grants("buckets", "read", "esm:download-part"));
        BOOST_TEST(grants("buckets", "read", "esm:complete-download"));

        // And reading is not writing.
        BOOST_TEST(!grants("buckets", "read", "esm:put-object"));
        BOOST_TEST(!grants("buckets", "read", "esm:create-upload"));
    }

    BOOST_AUTO_TEST_CASE(EveryLevelInTheTableIsReachableFromItsKind) {

        // What an error message offers has to be what the table actually holds, or the message sends
        // somebody to a level that does not work.
        for (const auto &kind: {"queues", "topics", "buckets", "secrets", "tables"}) {
            BOOST_TEST_CONTEXT(kind) {
                const auto levels = AccessLevelsFor(kind);
                BOOST_TEST(!levels.empty());
                for (const auto &level: levels) {
                    BOOST_TEST_CONTEXT(level) { BOOST_TEST(!PermissionsFor(kind, level).empty()); }
                }
            }
        }
        // A route is reached over HTTP, not through an access level; and a kind that does not exist
        // has none either.
        BOOST_TEST(AccessLevelsFor("routes").empty());
        BOOST_TEST(AccessLevelsFor("streams").empty());
    }

    // A secret is the one kind with a single verb, and the only one an application may name but
    // never own: its value cannot travel in a file that ships inside the artifact. So the table
    // holds exactly one row for it, and `creates` refuses it saying where the value goes instead.
    BOOST_AUTO_TEST_CASE(ASecretIsReadAndNeverCreated) {

        BOOST_TEST(grants("secrets", "read", "ess:get-secret"));
        BOOST_TEST((AccessLevelsFor("secrets") == std::vector<std::string>{"read"}));

        // No ERN lookup beside it, unlike every other kind: ess:get-secret takes the secret's name,
        // which is what a deployment's configuration can carry and what stays the same between
        // environments, so there is no name-to-ERN step for the access level to grant.
        BOOST_TEST(PermissionsFor("secrets", "read").size() == 1u);

        // And not the listing. What an application needs is the value of a secret it was told the
        // name of - ESS answers list-secrets by filtering to what the caller may read, so holding
        // it would turn a narrow grant into a map of what the grant does not cover.
        BOOST_TEST(!grants("secrets", "read", "ess:list-secrets"));

        const auto used = Read(parse(R"({"version": 1, "uses": {
            "secrets": [{"name": "suppliers-username", "access": "read"}]}})"));
        BOOST_TEST(used.error.empty());
        BOOST_REQUIRE(used.declaration.uses.size() == 1u);
        BOOST_TEST(used.declaration.uses.front().kind == "secrets");

        const auto created = Read(parse(R"({"version": 1, "creates": {
            "secrets": [{"name": "suppliers-password"}]}})"));
        BOOST_TEST(!created.error.empty());
        BOOST_TEST(created.error.find("ess create-secret") != std::string::npos, "unhelpful: " + created.error);

        // Written, the access levels that do not apply are refused by the table rather than by a
        // second list of rules: "secrets have no \"write\" access - only read".
        const auto written = Read(parse(R"({"version": 1, "uses": {
            "secrets": [{"name": "suppliers-password", "access": "write"}]}})"));
        BOOST_TEST(!written.error.empty());
        BOOST_TEST(written.error.find("only read") != std::string::npos, "unhelpful: " + written.error);
    }

    // ── merging a folder ────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(FilesAreMergedBySectionRatherThanByName) {

        // The split across files is the author's business: one of these named its file topics.json
        // and put a queue in it, which is why the merge cannot key on the filename.
        const auto topics = Read(parse(R"({"version": 1, "creates": {
            "topics": [{"name": "protokollierung-topic"}],
            "queues": [{"name": "file-logging-cancel-queue"}]}})"));
        const auto access = Read(parse(R"({"version": 1, "uses": {
            "topics": [{"name": "datenlieferant-updates", "access": "subscribe", "owner": "datenlieferant-service"}]}})"));
        BOOST_REQUIRE(topics.error.empty() && access.error.empty());

        const auto merged = Merge({topics.declaration, access.declaration});
        BOOST_REQUIRE(merged.error.empty());
        BOOST_TEST(merged.declaration.creates.size() == 2U);
        BOOST_TEST(merged.declaration.uses.size() == 1U);
    }

    BOOST_AUTO_TEST_CASE(TheSameResourceInTwoFilesIsRefused) {

        // Whichever of them a merge preferred, the other author would have been overruled without
        // being told.
        const auto one = Read(parse(R"({"version": 1, "creates": {"queues": [{"name": "orders"}]}})"));
        const auto two = Read(parse(R"({"version": 1, "creates": {"queues": [{"name": "orders"}]}})"));
        BOOST_REQUIRE(one.error.empty() && two.error.empty());

        const auto merged = Merge({one.declaration, two.declaration});
        BOOST_REQUIRE(!merged.error.empty());
        BOOST_TEST(merged.error.find("orders") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(OwningAResourceAndAlsoDeclaringItUsedIsRefused) {

        // The same name cannot be both mine and somebody else's, and the two halves mean opposite
        // things: one creates, the other only grants.
        const auto owns = Read(parse(R"({"version": 1, "creates": {"queues": [{"name": "orders"}]}})"));
        const auto uses = Read(parse(
                R"({"version": 1, "uses": {"queues": [{"name": "orders", "access": "produce"}]}})"));
        BOOST_REQUIRE(owns.error.empty() && uses.error.empty());

        BOOST_TEST(!Merge({owns.declaration, uses.declaration}).error.empty());
    }

    BOOST_AUTO_TEST_CASE(TheMergedObjectIsTheSameBytesWhateverOrderTheFilesWereRead) {

        // What lets a redeploy tell "unchanged" from "rewritten". Two runs over the same folder that
        // produced different bytes would make every deploy look like a change.
        const auto a = Read(parse(R"({"version": 1, "creates": {"queues": [{"name": "b"}]}})"));
        const auto b = Read(parse(R"({"version": 1, "creates": {"queues": [{"name": "a"}]}})"));
        BOOST_REQUIRE(a.error.empty() && b.error.empty());

        const auto forwards = Merge({a.declaration, b.declaration});
        const auto backwards = Merge({b.declaration, a.declaration});
        BOOST_REQUIRE(forwards.error.empty() && backwards.error.empty());

        BOOST_TEST(boost::json::serialize(Write(forwards.declaration))
                   == boost::json::serialize(Write(backwards.declaration)));
    }

    BOOST_AUTO_TEST_CASE(WhatIsWrittenIsWhatCanBeReadBack) {

        // The sidecar is written by one thing and read by another, so the two have to agree on the
        // shape without either being told about the other.
        const auto original = Read(parse(R"({
            "version": 1,
            "creates": {"queues": [{"name": "file-logging-cancel-queue"}]},
            "uses": {"buckets": [{"name": "transfer-server", "access": ["read", "subscribe"], "owner": "ftp-server"}]}
        })"));
        BOOST_REQUIRE(original.error.empty());

        const auto roundTripped = Read(Write(original.declaration));
        BOOST_REQUIRE(roundTripped.error.empty());
        BOOST_TEST(roundTripped.declaration.creates == original.declaration.creates);
        BOOST_TEST(roundTripped.declaration.uses == original.declaration.uses);
    }

    // ── whose declaration is it ─────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(ADeclarationThatNamesAnotherApplicationIsRefused) {

        // The guard that was missing, and its absence cost a topic and a queue: nothing but the object
        // key tied a file to an application, so a declaration attributed to the wrong one had its
        // resources recorded against that one - which then deleted them on its next reconcile,
        // because they were recorded and no longer declared.
        const auto read = Read(parse(R"({
            "version": 1,
            "applicationId": "protocolizing",
            "creates": {"topics": [{"name": "protokollierung-topic"}]}
        })"));
        BOOST_REQUIRE(read.error.empty());
        BOOST_TEST(read.declaration.applicationId == "protocolizing");

        BOOST_TEST(Belongs(read.declaration, "protocolizing").empty());

        const auto refused = Belongs(read.declaration, "file-copy");
        BOOST_REQUIRE(!refused.empty());
        BOOST_TEST(refused.find("protocolizing") != std::string::npos);
        BOOST_TEST(refused.find("file-copy") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(ADeclarationThatNamesNoApplicationBelongsToWhoeverHoldsIt) {

        // Every file written before the field existed, including the two already stored. Refusing
        // those would have made the guard a migration rather than a guard.
        const auto read = Read(parse(R"({"version": 1, "creates": {"queues": [{"name": "orders"}]}})"));
        BOOST_REQUIRE(read.error.empty());
        BOOST_TEST(read.declaration.applicationId.empty());
        BOOST_TEST(Belongs(read.declaration, "anything-at-all").empty());
    }

    BOOST_AUTO_TEST_CASE(TheApplicationIdSurvivesAMergeAndARoundTrip) {

        const auto one = Read(parse(R"({"version": 1, "applicationId": "file-copy",
            "creates": {"buckets": [{"name": "file-delivery"}]}})"));
        const auto two = Read(parse(R"({"version": 1,
            "uses": {"topics": [{"name": "t", "access": "produce"}]}})"));
        BOOST_REQUIRE(one.error.empty() && two.error.empty());

        const auto merged = Merge({one.declaration, two.declaration});
        BOOST_REQUIRE(merged.error.empty());
        BOOST_TEST(merged.declaration.applicationId == "file-copy");

        const auto roundTripped = Read(Write(merged.declaration));
        BOOST_REQUIRE(roundTripped.error.empty());
        BOOST_TEST(roundTripped.declaration.applicationId == "file-copy");
    }

    BOOST_AUTO_TEST_CASE(FilesThatDisagreeAboutWhoseTheyAreAreRefused) {

        // Taking the last would be the same mistake one level down: a folder holding one file from
        // another service would quietly claim to be that service, or not, depending on the filename.
        const auto mine = Read(parse(R"({"version": 1, "applicationId": "file-copy",
            "creates": {"buckets": [{"name": "file-delivery"}]}})"));
        const auto theirs = Read(parse(R"({"version": 1, "applicationId": "protocolizing",
            "creates": {"topics": [{"name": "protokollierung-topic"}]}})"));
        BOOST_REQUIRE(mine.error.empty() && theirs.error.empty());

        const auto merged = Merge({mine.declaration, theirs.declaration});
        BOOST_REQUIRE(!merged.error.empty());
        BOOST_TEST(merged.error.find("file-copy") != std::string::npos);
        BOOST_TEST(merged.error.find("protocolizing") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(EveryFileSayingTheSameThingIsFine) {

        // Which is how a folder is written once the id is in each file: repeated, and agreeing.
        const auto one = Read(parse(R"({"version": 1, "applicationId": "file-copy",
            "creates": {"buckets": [{"name": "file-delivery"}]}})"));
        const auto two = Read(parse(R"({"version": 1, "applicationId": "file-copy",
            "uses": {"topics": [{"name": "t", "access": "produce"}]}})"));
        BOOST_REQUIRE(one.error.empty() && two.error.empty());

        const auto merged = Merge({one.declaration, two.declaration});
        BOOST_REQUIRE(merged.error.empty());
        BOOST_TEST(merged.declaration.applicationId == "file-copy");
    }

    // ── settings ───────────────────────────────────────────────────────────

    // The four queues euclid-pim already declared settings for, which the deploy dropped: a
    // file that was right all along has to keep reading.
    BOOST_AUTO_TEST_CASE(AQueuesSettingsAreReadNotDropped) {
        const auto read = Read(parse(R"({"version":1,"creates":{"queues":[
            {"name":"export-katalogdaten-prepared-queue","visibility":900,"maxRetries":3},
            {"name":"export-katalogdaten-csv-publishing-queue","visibility":900,"maxMessageLength":1048576}]}})"));
        BOOST_TEST_REQUIRE(read.error.empty(), read.error);
        BOOST_TEST_REQUIRE(read.declaration.creates.size() == 2U);
        BOOST_TEST(read.declaration.creates[0].settings.at("visibility").as_int64() == 900);
        BOOST_TEST(read.declaration.creates[0].settings.at("maxRetries").as_int64() == 3);
        BOOST_TEST(!read.declaration.creates[0].settings.contains("name"));
    }

    // Passed through, a misspelt key was a queue at the default and a file that looked right.
    BOOST_AUTO_TEST_CASE(AMisspeltSettingIsRefusedAndTheRealOnesNamed) {
        const auto read = Read(parse(R"({"version":1,"creates":{"queues":[{"name":"q","visibilty":900}]}})"));
        BOOST_TEST(read.error.find("no setting \"visibilty\"") != std::string::npos, read.error);
        BOOST_TEST(read.error.find("visibility") != std::string::npos, read.error);
    }

    // 0 is the value that made every received message visible again at once.
    BOOST_AUTO_TEST_CASE(AVisibilityOfZeroIsRefused) {
        const auto read = Read(parse(R"({"version":1,"creates":{"queues":[{"name":"q","visibility":0}]}})"));
        BOOST_TEST(read.error.find("\"visibility\" has to be a whole number from 1") != std::string::npos, read.error);
    }

    BOOST_AUTO_TEST_CASE(ASettingOfTheWrongTypeIsRefused) {
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"queues":[{"name":"q","visibility":"900"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"queues":[{"name":"q","priority":"URGENT"}]}})")).error.empty());
        BOOST_TEST(Read(parse(R"({"version":1,"creates":{"queues":[{"name":"q","priority":"LOW"}]}})")).error.empty());
    }

    BOOST_AUTO_TEST_CASE(AQueueCannotBeItsOwnDeadLetterQueue) {
        const auto read = Read(parse(R"({"version":1,"creates":{"queues":[{"name":"q","deadLetterQueue":"q"}]}})"));
        BOOST_TEST(read.error.find("names the queue itself") != std::string::npos, read.error);
    }

    // Topics have two settings and buckets none, and each says so rather than ignoring the key.
    BOOST_AUTO_TEST_CASE(EachKindAcceptsOnlyItsOwnSettings) {
        BOOST_TEST(Read(parse(R"({"version":1,"creates":{"topics":[{"name":"t","retention":86400}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"topics":[{"name":"t","visibility":30}]}})")).error.empty());
        const auto bucket = Read(parse(R"({"version":1,"creates":{"buckets":[{"name":"b","encryption":true}]}})"));
        BOOST_TEST(bucket.error.find("they take none") != std::string::npos, bucket.error);
    }

    BOOST_AUTO_TEST_CASE(SettingsSurviveAWriteAndAReadBack) {
        const auto read = Read(parse(R"({"version":1,"creates":{"queues":[{"name":"q","visibility":600,"deadLetterQueue":"q-dlq"},{"name":"q-dlq"}]}})"));
        BOOST_TEST_REQUIRE(read.error.empty(), read.error);
        const auto again = Read(Write(read.declaration));
        BOOST_TEST_REQUIRE(again.error.empty(), again.error);
        BOOST_TEST((again.declaration.creates == read.declaration.creates));
    }

    // ── what a file may say ────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(AUsedObjectIsReachedNotConfigured) {
        const auto read = Read(parse(R"({"version":1,"uses":{"queues":[{"name":"q","access":"consume","visibility":30}]}})"));
        BOOST_TEST(read.error.find("unknown field \"visibility\"") != std::string::npos, read.error);
    }

    // A skipped "create" is a deploy that comes up having made nothing.
    BOOST_AUTO_TEST_CASE(AMisspeltSectionIsRefusedNotSkipped) {
        const auto read = Read(parse(R"({"version":1,"create":{"queues":[{"name":"q"}]}})"));
        BOOST_TEST(read.error.find("unknown section \"create\"") != std::string::npos, read.error);
    }

    BOOST_AUTO_TEST_CASE(ANameIsANameAndNotAnErn) {
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"queues":[{"name":"ern:eqs:eu-central-1:000000000000:production:queue:q"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"uses":{"queues":[{"name":"my queue","access":"consume"}]}})")).error.empty());
    }

    // ── a folder ───────────────────────────────────────────────────────────

    namespace {
        struct Folder {
            std::filesystem::path path = std::filesystem::temp_directory_path() / ("euclid-folder-" + std::to_string(::getpid()));
            Folder() { std::filesystem::create_directories(path); }
            ~Folder() { std::filesystem::remove_all(path); }
            void write(const std::string &name, const std::string &content) const { std::ofstream(path / name, std::ios::binary) << content; }
        };
    }// namespace

    BOOST_AUTO_TEST_CASE(AFolderOfFilesBecomesOneDeclaration) {
        const Folder folder;
        folder.write("queues.json", R"({"version":1,"applicationId":"orders","creates":{"queues":[{"name":"orders-in","visibility":300}]}})");
        folder.write("access.json", R"({"version":1,"applicationId":"orders","uses":{"topics":[{"name":"prices","access":"subscribe"}]}})");
        folder.write("README.md", "not a declaration");

        const auto loaded = LoadFolder(folder.path);
        BOOST_TEST_REQUIRE(loaded.error.empty(), loaded.error);
        BOOST_TEST(loaded.present);
        BOOST_TEST(loaded.files == 2U);
        BOOST_TEST(loaded.declaration.applicationId == "orders");
        BOOST_TEST_REQUIRE(loaded.declaration.creates.size() == 1U);
        BOOST_TEST(loaded.declaration.creates[0].settings.at("visibility").as_int64() == 300);
        BOOST_TEST(loaded.declaration.uses.size() == 1U);
    }

    // Most applications declare nothing; that is not a failure, and the caller decides what it means.
    BOOST_AUTO_TEST_CASE(AMissingFolderIsNotAnError) {
        const auto loaded = LoadFolder(std::filesystem::temp_directory_path() / "euclid-no-such-folder");
        BOOST_TEST(!loaded.present);
        BOOST_TEST(loaded.error.empty());
    }

    // What every Windows editor writes by default, and invisible.
    BOOST_AUTO_TEST_CASE(AByteOrderMarkIsSkipped) {
        const Folder folder;
        folder.write("queues.json", "\xEF\xBB\xBF" R"({"version":1,"creates":{"queues":[{"name":"q"}]}})");
        const auto loaded = LoadFolder(folder.path);
        BOOST_TEST(loaded.error.empty(), loaded.error);
    }

    BOOST_AUTO_TEST_CASE(AProblemNamesItsFile) {
        const Folder folder;
        folder.write("queues.json", R"({"version":1,"creates":{"queues":[{"name":"q","visibilty":1}]}})");
        const auto loaded = LoadFolder(folder.path);
        BOOST_TEST(loaded.error.starts_with("queues.json: "), loaded.error);
    }

    // ── applying ───────────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(WhoseAnObjectIsDecidesWhatHappensToIt) {
        BOOST_TEST((ClaimOn(false, {}, "orders") == Claim::Create));
        BOOST_TEST((ClaimOn(true, {}, "orders") == Claim::Adopt));
        BOOST_TEST((ClaimOn(true, {{kOwnerTag, "orders"}}, "orders") == Claim::Mine));
        // The deploy used to record such an object as owned, for the owner's reconcile to delete.
        BOOST_TEST((ClaimOn(true, {{kOwnerTag, "billing"}}, "orders") == Claim::Clash));
    }

    BOOST_AUTO_TEST_CASE(OnlyWhatIsStillOursIsEverRemoved) {
        BOOST_TEST(MayRemove({{kOwnerTag, "orders"}}, "orders", false));
        // Made by the deploy before the tag existed: the record is the claim.
        BOOST_TEST(MayRemove({}, "orders", true));
        BOOST_TEST(!MayRemove({}, "orders", false));
        // Taken over since: somebody else's, whatever the record says.
        BOOST_TEST(!MayRemove({{kOwnerTag, "billing"}}, "orders", true));
    }

    BOOST_AUTO_TEST_CASE(ApplyingSettingsChangesOnlyWhatIsNamed) {
        Euclid::Database::Entity::EQS::Queue queue;
        queue.maxReceiveCount = 7;// changed by hand, and not named in the file
        const auto changes = ApplyQueueSettings(queue, boost::json::object{{"visibility", 900}, {"priority", "HIGH"}});
        BOOST_TEST(queue.visibility == 900L);
        BOOST_TEST((queue.priority == Euclid::Database::Entity::EQS::MessagePriority::HIGH));
        BOOST_TEST(queue.maxReceiveCount == 7L);
        BOOST_TEST_REQUIRE(changes.size() == 2U);
        BOOST_TEST(changes[0] == "visibility 30 -> 900");
        BOOST_TEST(changes[1] == "priority MEDIUM -> HIGH");
    }

    BOOST_AUTO_TEST_CASE(ApplyingTheSameSettingsAgainChangesNothing) {
        Euclid::Database::Entity::EQS::Queue queue;
        const boost::json::object settings{{"visibility", 600}, {"maxRetries", 5}};
        std::ignore = ApplyQueueSettings(queue, settings);
        BOOST_TEST(ApplyQueueSettings(queue, settings).empty());
    }

    BOOST_AUTO_TEST_CASE(ADeadLetterQueueIsLinkedByItsErn) {
        Euclid::Database::Entity::EQS::Queue queue;
        const auto changes = ApplyQueueSettings(queue, boost::json::object{{"deadLetterQueue", "q-dlq"}}, "ern:eqs:r:a:n:queue:q-dlq");
        BOOST_TEST(queue.deadLetterQueueErn == "ern:eqs:r:a:n:queue:q-dlq");
        BOOST_TEST(changes.size() == 1U);
    }

    BOOST_AUTO_TEST_CASE(ATopicTakesItsTwoSettings) {
        Euclid::Database::Entity::ENS::Topic topic;
        const auto changes = ApplyTopicSettings(topic, boost::json::object{{"retention", 86400}});
        BOOST_TEST(topic.retentionPeriod == 86400L);
        BOOST_TEST(changes.size() == 1U);
    }

    // ── the levels that replace the hand-written pim-* roles ────────────────

    BOOST_AUTO_TEST_CASE(DeletingMovingAndRenamingAreLevelsOfTheirOwn) {
        BOOST_TEST(grants("buckets", "delete", "esm:delete-object"));
        BOOST_TEST(grants("buckets", "delete", "esm:delete-objects"));
        BOOST_TEST(grants("buckets", "move", "esm:copy-object"));
        BOOST_TEST(grants("buckets", "move", "esm:move-object"));
        BOOST_TEST(grants("buckets", "rename", "esm:rename-object"));
        BOOST_TEST(grants("queues", "control", "eqs:stop-queue"));
        BOOST_TEST(grants("queues", "control", "eqs:start-queue"));
        // A writer is not a remover: dropping files into a bucket is not removing what others put there.
        BOOST_TEST(!grants("buckets", "write", "esm:delete-object"));
    }

    BOOST_AUTO_TEST_CASE(OwningIsEveryLevelOfTheKind) {
        for (const auto &level: AccessLevelsFor("buckets")) {
            for (const auto &permission: PermissionsFor("buckets", level)) {
                BOOST_TEST(grants("buckets", kOwnAccess, permission), permission);
            }
        }
        BOOST_TEST(grants("queues", kOwnAccess, "eqs:send-message"));
        BOOST_TEST(grants("queues", kOwnAccess, "eqs:receive-messages"));
        BOOST_TEST(grants("queues", kOwnAccess, "eqs:stop-queue"));
        // Nobody owns a secret.
        BOOST_TEST(PermissionsFor("secrets", kOwnAccess).empty());
    }

    BOOST_AUTO_TEST_CASE(OwnIsNotSomethingAUsesEntryCanAskFor) {
        const auto read = Read(parse(R"({"version":1,"uses":{"buckets":[{"name":"b","access":"own"}]}})"));
        BOOST_TEST(read.error.find("\"own\" is what creates means") != std::string::npos, read.error);
    }

    // ── tables and routes ──────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(ATableIsDeclaredWithItsKey) {
        const auto read = Read(parse(R"({"version":1,"creates":{"tables":[
            {"name":"libri-product-export","partitionKey":"exportStartTime","sortKey":"composedSortKey"},
            {"name":"datenlieferant","partitionKey":"datenlieferantId","partitionKeyType":"string"}]}})"));
        BOOST_TEST_REQUIRE(read.error.empty(), read.error);
        BOOST_TEST(read.declaration.creates.size() == 2U);
    }

    BOOST_AUTO_TEST_CASE(ATableWithoutAKeyIsRefused) {
        const auto read = Read(parse(R"({"version":1,"creates":{"tables":[{"name":"t"}]}})"));
        BOOST_TEST(read.error.find("needs \"partitionKey\"") != std::string::npos, read.error);
    }

    BOOST_AUTO_TEST_CASE(AKeyThatCannotBeAKeyIsRefused) {
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"tables":[{"name":"t","partitionKey":"$id"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"tables":[{"name":"t","partitionKey":"a","sortKeyType":"number"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"tables":[{"name":"t","partitionKey":"a","sortKey":"a"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"tables":[{"name":"t","partitionKey":"a","partitionKeyType":"date"}]}})")).error.empty());
    }

    BOOST_AUTO_TEST_CASE(ATableIsReadOrWritten) {
        BOOST_TEST(grants("tables", "read", "ekv:query"));
        BOOST_TEST(grants("tables", "write", "ekv:put-item"));
        BOOST_TEST(grants("tables", "write", "ekv:delete-item"));
        BOOST_TEST(!grants("tables", "read", "ekv:put-item"));
        BOOST_TEST(Read(parse(R"({"version":1,"uses":{"tables":[{"name":"t","access":["read","write"]}]}})")).error.empty());
    }

    BOOST_AUTO_TEST_CASE(ARouteIsDeclaredWithItsPath) {
        const auto read = Read(parse(R"({"version":1,"creates":{"routes":[
            {"name":"suppliers-api","path":"/api/datenlieferant","methods":["get","POST"],"authentication":"EUCLID","timeout":300}]}})"));
        BOOST_TEST_REQUIRE(read.error.empty(), read.error);
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"routes":[{"name":"r"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"routes":[{"name":"r","path":"api"}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"routes":[{"name":"r","path":"/a","methods":["FETCH"]}]}})")).error.empty());
        BOOST_TEST(!Read(parse(R"({"version":1,"creates":{"routes":[{"name":"r","path":"/a","timeout":7200}]}})")).error.empty());
    }

    // A route is published by the application it reaches.
    BOOST_AUTO_TEST_CASE(ARouteIsNotSomethingAnApplicationUses) {
        const auto read = Read(parse(R"({"version":1,"uses":{"routes":[{"name":"r","access":"read"}]}})"));
        BOOST_TEST(read.error.find("is a route") != std::string::npos, read.error);
    }

BOOST_AUTO_TEST_SUITE_END()
