#define BOOST_TEST_MODULE SecretTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/ess/Secret.h>
#include <euclid/database/repository/ess/MongoEssRepository.h>

using Euclid::Core::CryptoUtils;
using Euclid::Database::MongoEssRepository;
using Euclid::Database::Entity::ESS::Secret;

// A secret is stored as ciphertext and nothing else: the module encrypts before it writes and
// decrypts only when somebody asks for the value by name. What has to hold is that the row carries
// the ciphertext and the key it was written under across the database and back - a secret whose
// key ERN was lost is a secret nobody can read again - and that the encryption itself is a real
// round trip rather than something that happens to look like one.

namespace {

    Secret demoSecret() {
        Secret secret;
        secret.accountId = "000000000000";
        secret.region = "eu-central-1";
        secret.nameSpace = "development";
        secret.name = "db-password";
        secret.ern = "ern:ess:eu-central-1:000000000000:development:secret:db-password";
        secret.description = "production postgres";
        secret.encryptionKeyErn = "ern:ekm:eu-central-1:000000000000:key:ess-development";
        secret.version = 3;
        secret.tags = {{"system", "payroll"}};
        return secret;
    }

}// namespace

BOOST_AUTO_TEST_SUITE(SecretTest)

    BOOST_AUTO_TEST_CASE(ASecretSurvivesABsonRoundTrip) {

        auto secret = demoSecret();
        secret.value = "Zd9VYXR0n/bKA3h61TXcg+eZQuRAfukE76NZXO/5szOF7UfIOMDGoz29kzDY";

        const auto restored = Secret::fromDocument(secret.toDocument().view());

        BOOST_TEST(restored.name == secret.name);
        BOOST_TEST(restored.ern == secret.ern);
        BOOST_TEST(restored.description == secret.description);
        BOOST_TEST(restored.value == secret.value);
        BOOST_TEST(restored.version == 3);
        BOOST_TEST(restored.tags.at("system") == "payroll");

        // The one field whose loss cannot be repaired: without it there is no way to know which
        // key the stored bytes were written under, and a secret nobody can decrypt is gone.
        BOOST_TEST(restored.encryptionKeyErn == secret.encryptionKeyErn);
    }

    BOOST_AUTO_TEST_CASE(WhatIsStoredIsCiphertextAndComesBackWhole) {

        const auto keyMaterial = CryptoUtils::Base64Decode(CryptoUtils::GenerateAes256Key());
        const std::string plaintext = "sup3r-s3cret-p@ss";

        // Exactly what the module does on the way in: encrypt, then base64 for a BSON string.
        auto secret = demoSecret();
        secret.value = CryptoUtils::Base64Encode(CryptoUtils::AesGcmEncrypt(keyMaterial, plaintext));

        BOOST_TEST(secret.value != plaintext);
        BOOST_TEST(!secret.value.contains(plaintext));

        const auto restored = Secret::fromDocument(secret.toDocument().view());
        BOOST_TEST(CryptoUtils::AesGcmDecrypt(keyMaterial, CryptoUtils::Base64Decode(restored.value)) == plaintext);
    }

    BOOST_AUTO_TEST_CASE(AValueWrittenUnderOneKeyDoesNotOpenUnderAnother) {

        const auto keyMaterial = CryptoUtils::Base64Decode(CryptoUtils::GenerateAes256Key());
        const auto otherKey = CryptoUtils::Base64Decode(CryptoUtils::GenerateAes256Key());
        const auto ciphertext = CryptoUtils::AesGcmEncrypt(keyMaterial, "sup3r-s3cret-p@ss");

        // GCM authenticates as well as encrypts, so the wrong key is refused rather than yielding
        // plausible rubbish - which is what lets the module report "this was not written under the
        // key it names" instead of handing an application a corrupt password.
        BOOST_CHECK_THROW(std::ignore = CryptoUtils::AesGcmDecrypt(otherKey, ciphertext), std::exception);
    }

    BOOST_AUTO_TEST_CASE(TheRepositoryKeepsOneRowPerNameInANamespace) {

        Euclid::Database::Database::instance().initializeMemory();
        MongoEssRepository repository;
        auto secret = demoSecret();
        secret.value = "first";
        std::ignore = repository.upsertSecret(secret);

        // The same secret rotated, not a second secret: the name is what an application asks for,
        // and two rows under one name would make which value it got a matter of luck.
        secret.value = "second";
        secret.version = 4;
        std::ignore = repository.upsertSecret(secret);

        BOOST_TEST(repository.countSecrets("000000000000", "development", "") == 1);
        BOOST_TEST_REQUIRE(repository.secretExists("000000000000", "development", "db-password"));
        BOOST_TEST(repository.findSecretByName("000000000000", "development", "db-password")->value == "second");
        BOOST_TEST(repository.findSecretByErn(secret.ern).has_value());

        // The same name in another namespace is another secret entirely - development and
        // production both having a "db-password" is the ordinary case.
        auto other = demoSecret();
        other.nameSpace = "production";
        other.ern = "ern:ess:eu-central-1:000000000000:production:secret:db-password";
        other.value = "production";
        std::ignore = repository.upsertSecret(other);

        BOOST_TEST(repository.countSecrets("000000000000", "development", "") == 1);
        BOOST_TEST(repository.findSecretByName("000000000000", "production", "db-password")->value == "production");
        BOOST_TEST(repository.listSecrets("000000000000", "", "", 0, 0, "name", "asc").size() == 2U);
        BOOST_TEST(repository.listSecrets("000000000000", "development", "db-", 0, 0, "name", "asc").size() == 1U);
        BOOST_TEST(repository.listSecrets("000000000000", "development", "zzz", 0, 0, "name", "asc").empty());
    }

    BOOST_AUTO_TEST_CASE(DeletingTakesTheSecretAndLeavesTheOthers) {

        Euclid::Database::Database::instance().initializeMemory();
        MongoEssRepository repository;
        auto secret = demoSecret();
        secret.value = "first";
        std::ignore = repository.upsertSecret(secret);

        auto other = demoSecret();
        other.name = "api-token";
        other.ern = "ern:ess:eu-central-1:000000000000:development:secret:api-token";
        other.value = "second";
        std::ignore = repository.upsertSecret(other);

        BOOST_TEST(repository.deleteSecret("000000000000", "development", "db-password") == 1);
        BOOST_TEST(!repository.secretExists("000000000000", "development", "db-password"));
        BOOST_TEST(repository.secretExists("000000000000", "development", "api-token"));

        // Deleting what is not there is not an error, so a retried delete does not fail the second
        // time.
        BOOST_TEST(repository.deleteSecret("000000000000", "development", "db-password") == 0);
    }


// One pass over the whole write path, for the seam itself: the repository written for MongoDB,
// running against the in-memory document store with no database anywhere. The operators it leans
// on - $set, $setOnInsert, $regex with a sort and a page - are the ones a store that only half
// implemented them would get quietly wrong, so they are asserted here rather than assumed.
BOOST_AUTO_TEST_CASE(TheMongoRepositoryRunsAgainstTheInMemoryStore) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEssRepository repository;

    auto secret = demoSecret();
    secret.value = "first";
    const auto stored = repository.upsertSecret(secret);

    BOOST_TEST(stored.name == "db-password");
    BOOST_TEST(stored.value == "first");
    BOOST_TEST(!stored.oid.empty());
    BOOST_TEST(stored.encryptionKeyErn == secret.encryptionKeyErn);

    // An upsert, not an insert: writing the same name again has to replace the value rather than
    // leave the first one standing.
    secret.value = "second";
    secret.version = 2;
    const auto updated = repository.upsertSecret(secret);
    BOOST_TEST(updated.value == "second");
    BOOST_TEST(updated.version == 2);
    BOOST_TEST(repository.countSecrets("000000000000", "development", "") == 1);

    // $setOnInsert: the creation date is the one the row was created with, not the one the second
    // write carried.
    BOOST_TEST((updated.created == stored.created));

    BOOST_TEST_REQUIRE(repository.secretExists("000000000000", "development", "db-password"));
    BOOST_TEST(repository.findSecretByName("000000000000", "development", "db-password")->value == "second");
    BOOST_TEST(repository.findSecretByErn(secret.ern).has_value());
    BOOST_TEST(!repository.findSecretByName("000000000000", "development", "nothing").has_value());

    // A prefix listing with a sort and a page, which is the $regex/sort/limit/skip path.
    auto other = demoSecret();
    other.name = "db-user";
    other.ern = "ern:ess:eu-central-1:000000000000:development:secret:db-user";
    other.value = "x";
    std::ignore = repository.upsertSecret(other);

    auto token = demoSecret();
    token.name = "api-token";
    token.ern = "ern:ess:eu-central-1:000000000000:development:secret:api-token";
    token.value = "y";
    std::ignore = repository.upsertSecret(token);

    const auto page = repository.listSecrets("000000000000", "development", "db-", 0, 0, "name", "asc");
    BOOST_TEST_REQUIRE(page.size() == 2U);
    BOOST_TEST(page[0].name == "db-password");
    BOOST_TEST(page[1].name == "db-user");
    BOOST_TEST(repository.countSecrets("000000000000", "development", "db-") == 2);
    BOOST_TEST(repository.listSecrets("000000000000", "development", "", 1, 1, "name", "asc").size() == 1U);

    BOOST_TEST(repository.deleteSecret("000000000000", "development", "db-user") == 1);
    BOOST_TEST(repository.countSecrets("000000000000", "development", "") == 2);
}

BOOST_AUTO_TEST_SUITE_END()
