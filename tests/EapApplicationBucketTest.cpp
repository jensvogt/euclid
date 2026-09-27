// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EapApplicationBucketTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <string>

// Euclid includes
#include <EapServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/eam/MongoEamRepository.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

using Euclid::Core::Configuration;
using Euclid::Database::MongoEamRepository;
using Euclid::Database::MongoEsmRepository;
using Euclid::Database::Entity::ESM::Bucket;
using Euclid::EAP::EapServer;

// EAP creates the bucket applications are deployed from when it starts, because the alternative was
// an operator having to know to run create-bucket first and a create-application refused with
// "Bucket not found" when they did not.
//
// Three things about it can go wrong quietly.
//
// It could be created again on every start - upsertBucket() writes the whole document from the copy
// handed to it, so a second write would reset the object count, the size and any encryption key the
// bucket had been given, and nothing would say so.
//
// It could be created somewhere nothing looks for it. A bucket name is unique within
// (accountId, nameSpace), and every lookup that resolves a name - list-buckets, upload-file,
// create-application - is scoped to the namespace the request was made in. One bucket at the account
// root is therefore invisible to a client working in "development", which is how this was first
// written and why these cases exist.
//
// And it could be hidden and then hidden again: the internal flag is an operator's to take off.

namespace {

    constexpr auto kAccount = "000000000000";

    // The configuration is a process-wide singleton and load() replaces it wholesale, so each case
    // states the whole of what it needs rather than inheriting whatever ran before it.
    void configure(const std::string &json) {
        const auto path = std::filesystem::temp_directory_path() / "euclid-eap-application-bucket-test.json";
        std::ofstream file(path, std::ios::trunc);
        BOOST_TEST_REQUIRE(file.is_open());
        file << json;
        file.close();
        Configuration::instance().load(path);
    }

    struct Repositories {
        MongoEsmRepository storage;
        MongoEamRepository identities;
    };

    Repositories freshRepositories() {
        Euclid::Database::Database::instance().initializeMemory();
        // Both constructors are explicit, so the members are built rather than brace-initialised.
        return Repositories{MongoEsmRepository{}, MongoEamRepository{}};
    }

    void ensure(Repositories &repositories) {
        EapServer::EnsureApplicationBucket(repositories.storage, repositories.identities);
    }

    // A namespace as EAM holds it, which is what an installation that created one at run time has
    // rather than an entry in euclid.json.
    void createNamespace(MongoEamRepository &identities, const std::string &name) {
        Euclid::Database::Entity::EAM::Namespace ns;
        ns.accountId = kAccount;
        ns.name = name;
        ns.ern = std::string("ern:eam:eu-central-1:") + kAccount + ":namespace:" + name;
        std::ignore = identities.upsertNamespace(ns);
    }

    std::vector<Bucket> allBuckets(const MongoEsmRepository &storage, const std::string &accountId = kAccount) {
        // includeInternal, or a bucket created and hidden would read as one never created.
        return storage.listBuckets(accountId, "", "", 0, 0, "", "asc", true);
    }

}// namespace

BOOST_AUTO_TEST_CASE(TheBucketIsCreatedWhereADeploymentLooksForIt) {

    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000"]}})");
    auto repositories = freshRepositories();

    ensure(repositories);

    // In the configured account, at its root, under the name the documented workflow has always
    // used - which is the lookup create-application makes for a request that names no namespace.
    const auto bucket = repositories.storage.findBucketByName(kAccount, "", "apps");
    BOOST_TEST_REQUIRE(bucket.has_value());
    BOOST_TEST(bucket->name == "apps");
    BOOST_TEST(bucket->ern == "ern:esm:eu-central-1:000000000000::bucket:apps");
    BOOST_TEST(bucket->accountId == "000000000000");
    BOOST_TEST(bucket->region == "eu-central-1");
    BOOST_TEST(bucket->nameSpace == "");

    // Euclid's own plumbing, so list-buckets and the bucket count leave it out. Hidden is all that
    // means: an artifact is uploaded into it by name like any other object.
    BOOST_TEST(bucket->internal);

    // Not a person. Whoever started the module did not ask for this bucket.
    BOOST_TEST(bucket->owner == "eap");
}

BOOST_AUTO_TEST_CASE(EveryDeclaredNamespaceGetsOneOfItsOwn) {

    // The shipped configuration declares three. A client working in "development" resolves "apps"
    // in "development", so the bucket at the account root is a different bucket to it - not a
    // permission problem, a different row, and the reason one is not enough.
    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000"],
                  "namespaces": ["development", "integration", "production"]}})");
    auto repositories = freshRepositories();

    ensure(repositories);

    for (const auto *nameSpace: {"", "development", "integration", "production"}) {
        const auto bucket = repositories.storage.findBucketByName(kAccount, nameSpace, "apps");
        BOOST_TEST_REQUIRE(bucket.has_value(), "no apps bucket in namespace '" << nameSpace << "'");
        BOOST_TEST(bucket->nameSpace == nameSpace);
        BOOST_TEST(bucket->internal);
    }

    // The namespace is part of the ERN, so the four are four distinct resources.
    BOOST_TEST(repositories.storage.findBucketByName(kAccount, "development", "apps")->ern
               == "ern:esm:eu-central-1:000000000000:development:bucket:apps");

    // And nowhere else: four, not one per namespace anybody might one day invent.
    BOOST_TEST(allBuckets(repositories.storage).size() == 4U);
}

BOOST_AUTO_TEST_CASE(ANamespaceThatOnlyExistsInEamGetsOneToo) {

    // A namespace created through EAM after euclid.json was written. The configuration cannot know
    // about it, and a deployment into it needs the bucket just the same.
    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000"]}})");
    auto repositories = freshRepositories();
    createNamespace(repositories.identities, "staging");

    ensure(repositories);

    BOOST_TEST(repositories.storage.findBucketByName(kAccount, "staging", "apps").has_value());
    BOOST_TEST(repositories.storage.findBucketByName(kAccount, "", "apps").has_value());
    BOOST_TEST(allBuckets(repositories.storage).size() == 2U);
}

BOOST_AUTO_TEST_CASE(ANamespaceNamedTwiceIsStillOneBucket) {

    // Declared in the configuration and created in EAM, which is the normal state of an installation
    // rather than an edge case - and upserting the same bucket twice would reset the first one.
    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000"],
                  "namespaces": ["development"]}})");
    auto repositories = freshRepositories();
    createNamespace(repositories.identities, "development");

    ensure(repositories);

    BOOST_TEST(allBuckets(repositories.storage).size() == 2U);
}

BOOST_AUTO_TEST_CASE(EachConfiguredAccountGetsItsOwn) {

    // A bucket name is unique within an account, and create-application resolves the name in the
    // deployer's own - so an account without this bucket cannot deploy from it.
    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000", "111111111111"]}})");
    auto repositories = freshRepositories();

    ensure(repositories);

    BOOST_TEST(repositories.storage.findBucketByName("000000000000", "", "apps").has_value());
    BOOST_TEST(repositories.storage.findBucketByName("111111111111", "", "apps").has_value());

    // And nowhere else: an account nobody configured is not one this installation serves.
    BOOST_TEST(!repositories.storage.findBucketByName("222222222222", "", "apps").has_value());
}

BOOST_AUTO_TEST_CASE(AStartAfterTheFirstChangesNothingAboutIt) {

    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000"]}})");
    auto repositories = freshRepositories();

    // A bucket that has been in use: artifacts in it, bytes on disk, encryption enabled, and the
    // internal flag taken back off by an operator who wanted to see it in their listing.
    ensure(repositories);
    auto stored = repositories.storage.findBucketByName(kAccount, "", "apps");
    BOOST_TEST_REQUIRE(stored.has_value());
    stored->objects = 17;
    stored->size = 4096;
    stored->encryptionKeyErn = "ern:ekm:eu-central-1:000000000000::key:artifacts";
    stored->internal = false;
    stored->tags = {{"owner", "platform"}};
    std::ignore = repositories.storage.upsertBucket(*stored);

    // The next start, and the one after that.
    ensure(repositories);
    ensure(repositories);

    const auto after = repositories.storage.findBucketByName(kAccount, "", "apps");
    BOOST_TEST_REQUIRE(after.has_value());
    BOOST_TEST(after->objects == 17);
    BOOST_TEST(after->size == 4096);
    BOOST_TEST(after->encryptionKeyErn == "ern:ekm:eu-central-1:000000000000::key:artifacts");
    BOOST_TEST(after->tags.contains("owner"));

    // Not re-hidden either. The bucket holds the installation's artifacts, and what an operator
    // decided about it is not something a restart gets to overrule.
    BOOST_TEST(!after->internal);

    // And no second bucket beside it.
    BOOST_TEST(allBuckets(repositories.storage).size() == 1U);
}

BOOST_AUTO_TEST_CASE(TheNameComesFromTheConfiguration) {

    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000"],
                  "modules": {"eap": {"bucket": "artifacts"}}}})");
    auto repositories = freshRepositories();

    ensure(repositories);

    BOOST_TEST(repositories.storage.findBucketByName(kAccount, "", "artifacts").has_value());
    BOOST_TEST(!repositories.storage.findBucketByName(kAccount, "", "apps").has_value());
}

BOOST_AUTO_TEST_CASE(ConfiguringNoNameCreatesNothing) {

    // How an installation that keeps its artifacts in a bucket of its own naming opts out.
    configure(R"({"euclid": {"region": "eu-central-1", "account-ids": ["000000000000"],
                  "modules": {"eap": {"bucket": ""}}}})");
    auto repositories = freshRepositories();

    ensure(repositories);

    BOOST_TEST(!repositories.storage.findBucketByName(kAccount, "", "apps").has_value());
    BOOST_TEST(allBuckets(repositories.storage).empty());
}

BOOST_AUTO_TEST_CASE(WithoutAConfiguredAccountNothingIsCreated) {

    // The account is part of the ERN. A bucket written with a hole where it belongs would not be
    // found by the lookup create-application makes, so it would be invisible rather than unused -
    // better to say so in the log and create nothing.
    configure(R"({"euclid": {"region": "eu-central-1"}})");
    auto repositories = freshRepositories();

    ensure(repositories);

    BOOST_TEST(allBuckets(repositories.storage, "").empty());
    BOOST_TEST(allBuckets(repositories.storage).empty());
}
