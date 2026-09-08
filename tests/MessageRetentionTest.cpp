#define BOOST_TEST_MODULE MessageRetentionTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>

using Euclid::Database::MongoEqsRepository;
using Euclid::Database::Entity::EQS::kDefaultRetentionPeriod;
using Euclid::Database::Entity::EQS::MessagePriority;

// A message nobody consumes used to stay forever, and the cost of that was not paid by the queue
// that collected it: every queue shares one collection, so one abandoned consumer made every send
// in the installation slower. Retention is the answer, and what this pins down is the part euclid
// decides - the expiry stamped on each message as it is sent. Removing the message afterwards is
// the database's job, done by the TTL index on that field, and not something a unit test can
// observe against the in-memory store.

namespace {

    using seconds = std::chrono::seconds;
    using system_clock = std::chrono::system_clock;

    // A queue per test, because MongoEqsRepository caches a queue's configuration in a static map
    // for thirty seconds and upsertQueue() does not invalidate it - so two tests sharing an ERN
    // would have the first one's retention answer the second one's send.
    std::string queueErn(const std::string &name) {
        return "ern:eqs:eu-central-1:000000000000:development:queue:" + name;
    }

    MongoEqsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEqsRepository{};
    }

    // A queue with a retention period of its own, or none at all when the period is zero.
    void addQueue(MongoEqsRepository &repo, const std::string &ern, const long retentionPeriod) {
        Euclid::Database::Entity::EQS::Queue queue;
        queue.ern = ern;
        queue.name = ern.substr(ern.rfind(':') + 1);
        queue.retentionPeriod = retentionPeriod;
        repo.upsertQueue(queue);
    }

    Euclid::Database::Entity::EQS::Message send(MongoEqsRepository &repo, const std::string &ern, const std::string &messageId) {
        return repo.sendMessage(messageId, "ern:eqs:eu-central-1:000000000000:message:" + messageId,
                                ern, "{}", {}, {}, MessagePriority::MIDDLE);
    }

    // How far ahead of now the message expires, in seconds - the send stamps it from its own clock
    // reading, so an exact comparison would be a race.
    long secondsUntilExpiry(const Euclid::Database::Entity::EQS::Message &message) {
        return std::chrono::duration_cast<seconds>(message.expiresAt - system_clock::now()).count();
    }

    // Within a few seconds of expected, which is all the precision a wall clock reading taken
    // inside the call can promise.
    bool isAbout(const long actual, const long expected) {
        return actual > expected - 3 && actual <= expected;
    }

    // Puts the setting back, so the order the tests run in cannot change what they mean.
    struct ConfiguredRetention {
        explicit ConfiguredRetention(const int seconds) {
            Euclid::Core::Configuration::instance().set("euclid.modules.eqs.retention-period", seconds);
        }
        ~ConfiguredRetention() {
            Euclid::Core::Configuration::instance().set("euclid.modules.eqs.retention-period",
                                                        static_cast<int>(kDefaultRetentionPeriod));
        }
    };

}// namespace

BOOST_AUTO_TEST_CASE(AQueueWithoutAPeriodOfItsOwnGetsTheInstallationDefault) {
    auto repo = freshRepository();
    const auto ern = queueErn("inherits");
    addQueue(repo, ern, 0);

    BOOST_TEST(isAbout(secondsUntilExpiry(send(repo, ern, "msg-default")), kDefaultRetentionPeriod));
}

BOOST_AUTO_TEST_CASE(AQueueKeepsItsOwnPeriod) {
    auto repo = freshRepository();
    const auto ern = queueErn("one-hour");
    addQueue(repo, ern, 3600);

    BOOST_TEST(isAbout(secondsUntilExpiry(send(repo, ern, "msg-hour")), 3600));
}

// The setting exists so an installation can decide without editing every queue, and so the queues
// written before retention existed - which are the ones that caused the problem - are governed by
// it rather than exempt from it.
BOOST_AUTO_TEST_CASE(TheInstallationDefaultIsConfigurable) {
    const ConfiguredRetention configured{120};
    auto repo = freshRepository();
    const auto ern = queueErn("configured");
    addQueue(repo, ern, 0);

    BOOST_TEST(isAbout(secondsUntilExpiry(send(repo, ern, "msg-configured")), 120));
}

// Nonsense in the configuration should leave messages expiring on the default rather than at once,
// which is what a zero or negative period would otherwise mean to a TTL index.
BOOST_AUTO_TEST_CASE(AnUnusableConfiguredPeriodFallsBackRatherThanExpiringImmediately) {
    const ConfiguredRetention configured{-1};
    auto repo = freshRepository();
    const auto ern = queueErn("nonsense");
    addQueue(repo, ern, 0);

    const auto message = send(repo, ern, "msg-negative");

    BOOST_TEST(secondsUntilExpiry(message) > 0);
    BOOST_TEST(isAbout(secondsUntilExpiry(message), kDefaultRetentionPeriod));
}

// A queue's own period governs its messages and nobody else's, which is what makes a short-lived
// queue possible alongside one that keeps its backlog for days.
BOOST_AUTO_TEST_CASE(EachQueuesMessagesExpireOnThatQueuesTerms) {
    auto repo = freshRepository();
    const auto shortLived = queueErn("short-lived");
    const auto longLived = queueErn("long-lived");
    addQueue(repo, shortLived, 60);
    addQueue(repo, longLived, 7200);

    BOOST_TEST(isAbout(secondsUntilExpiry(send(repo, shortLived, "msg-short")), 60));
    BOOST_TEST(isAbout(secondsUntilExpiry(send(repo, longLived, "msg-long")), 7200));
}
