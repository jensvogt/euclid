#define BOOST_TEST_MODULE TopicRetentionTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>

using Euclid::Database::MongoEnsRepository;
using Euclid::Database::Entity::ENS::kDefaultRetentionPeriod;

// A published message used to stay forever. A topic is fanned out at publish time, so nothing ever
// consumed one and nothing ever removed it - and every topic shares one collection, so the cost of
// one busy topic was paid by every publish in the installation. What this pins down is the part
// euclid decides: the expiry stamped on each message as it is published. Removing it afterwards is
// the database's job, done by the TTL index on that field, which a unit test against the in-memory
// store cannot observe.

namespace {

    using seconds = std::chrono::seconds;
    using system_clock = std::chrono::system_clock;

    MongoEnsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEnsRepository{};
    }

    std::string topicErn(const std::string &name) {
        return "ern:ens:eu-central-1:000000000000:development:topic:" + name;
    }

    // A topic with a retention period of its own, or none at all when the period is zero.
    void addTopic(MongoEnsRepository &repo, const std::string &ern, const long retentionPeriod) {
        Euclid::Database::Entity::ENS::Topic topic;
        topic.ern = ern;
        topic.name = ern.substr(ern.rfind(':') + 1);
        topic.retentionPeriod = retentionPeriod;
        repo.upsertTopic(topic);
    }

    Euclid::Database::Entity::ENS::Message publish(MongoEnsRepository &repo, const std::string &ern, const std::string &messageId) {
        return repo.publishMessage(messageId, "ern:ens:eu-central-1:000000000000:message:" + messageId, ern, "{}", {}, "MIDDLE");
    }

    // How far ahead of now the message expires, in seconds - the publish stamps it from its own
    // clock reading, so an exact comparison would be a race.
    long secondsUntilExpiry(const Euclid::Database::Entity::ENS::Message &message) {
        return std::chrono::duration_cast<seconds>(message.expiresAt - system_clock::now()).count();
    }

    bool isAbout(const long actual, const long expected) {
        return actual > expected - 3 && actual <= expected;
    }

    // Puts the setting back, so the order the tests run in cannot change what they mean.
    struct ConfiguredRetention {
        explicit ConfiguredRetention(const int seconds) {
            Euclid::Core::Configuration::instance().set("euclid.modules.ens.retention-period", seconds);
        }
        ~ConfiguredRetention() {
            Euclid::Core::Configuration::instance().set("euclid.modules.ens.retention-period",
                                                        static_cast<int>(kDefaultRetentionPeriod));
        }
    };

}// namespace

BOOST_AUTO_TEST_CASE(TheDefaultIsFourteenDays) {

    // The number itself, because it is a promise made in the documentation and in every shipped
    // configuration, and changing it silently would break both.
    BOOST_TEST(kDefaultRetentionPeriod == 14 * 24 * 60 * 60);
}

BOOST_AUTO_TEST_CASE(ATopicWithoutAPeriodOfItsOwnGetsTheInstallationDefault) {

    auto repo = freshRepository();
    const auto ern = topicErn("inherits");
    addTopic(repo, ern, 0);

    BOOST_TEST(isAbout(secondsUntilExpiry(publish(repo, ern, "msg-default")), kDefaultRetentionPeriod));
}

BOOST_AUTO_TEST_CASE(ATopicKeepsItsOwnPeriod) {

    auto repo = freshRepository();
    const auto ern = topicErn("one-hour");
    addTopic(repo, ern, 3600);

    BOOST_TEST(isAbout(secondsUntilExpiry(publish(repo, ern, "msg-hour")), 3600));
}

BOOST_AUTO_TEST_CASE(ATopicWithoutAPeriodFollowsTheConfiguredOne) {

    // Zero on the topic means "whatever the installation says", read at publish time - so an
    // operator who changes the setting does not have to touch every topic, or restart anything.
    const ConfiguredRetention configured{7200};

    auto repo = freshRepository();
    const auto ern = topicErn("follows-configuration");
    addTopic(repo, ern, 0);

    BOOST_TEST(isAbout(secondsUntilExpiry(publish(repo, ern, "msg-configured")), 7200));
}

BOOST_AUTO_TEST_CASE(ATopicsOwnPeriodBeatsTheConfiguredOne) {

    const ConfiguredRetention configured{7200};

    auto repo = freshRepository();
    const auto ern = topicErn("own-beats-configured");
    addTopic(repo, ern, 60);

    BOOST_TEST(isAbout(secondsUntilExpiry(publish(repo, ern, "msg-own")), 60));
}

BOOST_AUTO_TEST_CASE(AnUnusableConfiguredPeriodFallsBackToTheDefault) {

    // Zero and negative both mean "somebody set this to something that cannot be meant". Left as
    // given, a zero would stamp every message as expiring the moment it was published.
    const ConfiguredRetention configured{0};

    auto repo = freshRepository();
    const auto ern = topicErn("bad-configuration");
    addTopic(repo, ern, 0);

    BOOST_TEST(isAbout(secondsUntilExpiry(publish(repo, ern, "msg-bad")), kDefaultRetentionPeriod));
}

BOOST_AUTO_TEST_CASE(TheExpiryIsStoredWithTheMessage) {

    auto repo = freshRepository();
    const auto ern = topicErn("stored");
    addTopic(repo, ern, 3600);

    const auto published = publish(repo, ern, "msg-stored");
    const auto read = repo.findMessageById(published.messageId);

    BOOST_REQUIRE(read.has_value());
    BOOST_TEST(isAbout(secondsUntilExpiry(*read), 3600));
}

BOOST_AUTO_TEST_CASE(AMessageWithNoExpiryStoresNoField) {

    // What a message published before retention existed looks like: no field at all, which is what
    // a TTL index ignores. Written as the epoch it would be deleted the moment it was stored.
    Euclid::Database::Entity::ENS::Message message;
    message.messageId = "no-expiry";
    message.ern = "ern:ens:eu-central-1:000000000000:message:no-expiry";

    const auto document = message.ToDocument();
    BOOST_TEST(!document.view()["expiresAt"]);
}
