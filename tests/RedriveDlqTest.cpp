#define BOOST_TEST_MODULE RedriveDlqTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/repository/eqs/MemoryEqsRepository.h>

using Euclid::Database::MemoryEqsRepository;
using Euclid::Database::Entity::EQS::MessageStatus;

// redrive-dlq rests on two facts the repository has to get right, and both are easy to get subtly
// wrong:
//
//   - a queue is a dead letter queue only because other queues name it as theirs. Nothing on the
//     queue itself says so, so "is this a DLQ" is answered by asking who points at it - and an
//     ordinary queue must come back with nobody, which is what the command refuses on;
//   - a message moved to a dead letter queue has its queueErn overwritten, which destroys the only
//     record of where it had been. It carries sourceQueueErn now so a redrive can put it back
//     exactly there, which is what makes "the original queue" answerable when several queues share
//     one dead letter queue.

namespace {

    constexpr auto kOrders = "ern:eqs:eu-central-1:000000000000:development:queue:orders";
    constexpr auto kInvoices = "ern:eqs:eu-central-1:000000000000:development:queue:invoices";
    constexpr auto kDlq = "ern:eqs:eu-central-1:000000000000:development:queue:shared-dlq";
    constexpr auto kPlain = "ern:eqs:eu-central-1:000000000000:development:queue:plain";

    // A queue that sends its failures to dlqErn, or nowhere when that is empty.
    void addQueue(MemoryEqsRepository &repo, const std::string &ern, const std::string &dlqErn = {}) {
        Euclid::Database::Entity::EQS::Queue queue;
        queue.ern = ern;
        queue.name = ern.substr(ern.rfind(':') + 1);
        queue.deadLetterQueueErn = dlqErn;
        repo.upsertQueue(queue);
    }

    // A message sitting in the dead letter queue, recorded as having come from sourceErn.
    void addDeadMessage(MemoryEqsRepository &repo, const std::string &messageId, const std::string &sourceErn) {
        Euclid::Database::Entity::EQS::Message message;
        message.messageId = messageId;
        message.ern = "ern:eqs:eu-central-1:000000000000:message:" + messageId;
        message.queueErn = kDlq;
        message.sourceQueueErn = sourceErn;
        message.body = "{}";
        message.status = MessageStatus::AVAILABLE;
        message.receivedCount = 5;
        message.receiptHandle = "stale-handle";
        repo.upsertMessage(message);
    }

    // findMessageById() matches on oid, which nothing here sets. The store is keyed by messageId,
    // so that is what the tests look up by.
    std::optional<Euclid::Database::Entity::EQS::Message> findByMessageId(const MemoryEqsRepository &repo, const std::string &messageId) {
        for (const auto &message: repo.findAllMessages()) {
            if (message.messageId == messageId) return message;
        }
        return std::nullopt;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AnOrdinaryQueueHasNoSourceQueues) {
    MemoryEqsRepository repo;
    addQueue(repo, kPlain);
    addQueue(repo, kOrders, kDlq);

    // Empty is what the command turns into "this is not a dead letter queue".
    BOOST_TEST(repo.listSourceQueues(kPlain).empty());
}

BOOST_AUTO_TEST_CASE(ADeadLetterQueueNamesItsSources) {
    MemoryEqsRepository repo;
    addQueue(repo, kDlq);
    addQueue(repo, kOrders, kDlq);
    addQueue(repo, kInvoices, kDlq);

    const auto sources = repo.listSourceQueues(kDlq);

    BOOST_TEST(sources.size() == 2u);
}

BOOST_AUTO_TEST_CASE(RedriveMovesMessagesBackAndResetsThem) {
    MemoryEqsRepository repo;
    addQueue(repo, kDlq);
    addQueue(repo, kOrders, kDlq);
    addDeadMessage(repo, "m1", kOrders);

    BOOST_TEST(repo.redriveMessages(kDlq, kOrders, "") == 1L);

    const auto moved = findByMessageId(repo, "m1");
    BOOST_REQUIRE(moved.has_value());
    BOOST_TEST(moved->queueErn == kOrders);
    // Starts over as though new: nothing left of the attempts that killed it, and no origin to
    // return to, because it is back where it started.
    BOOST_TEST(moved->receivedCount == 0L);
    BOOST_TEST(moved->receiptHandle.empty());
    BOOST_TEST(moved->sourceQueueErn.empty());
    BOOST_TEST((moved->status == MessageStatus::AVAILABLE));
}

BOOST_AUTO_TEST_CASE(RedriveWithoutASourceFilterTakesEverything) {
    MemoryEqsRepository repo;
    addQueue(repo, kDlq);
    addQueue(repo, kOrders, kDlq);
    addDeadMessage(repo, "m1", kOrders);
    addDeadMessage(repo, "m2", kInvoices);
    addDeadMessage(repo, "m3", "");// no origin recorded

    BOOST_TEST(repo.redriveMessages(kDlq, kOrders, "") == 3L);
}

BOOST_AUTO_TEST_CASE(RedrivePerSourceTakesOnlyThatQueuesMessages) {
    MemoryEqsRepository repo;
    addQueue(repo, kDlq);
    addQueue(repo, kOrders, kDlq);
    addQueue(repo, kInvoices, kDlq);
    addDeadMessage(repo, "m1", kOrders);
    addDeadMessage(repo, "m2", kInvoices);

    BOOST_TEST(repo.redriveMessages(kDlq, kOrders, kOrders) == 1L);

    // The other one is untouched, still in the dead letter queue and still knowing where it came
    // from - which is what lets the second pass place it correctly.
    const auto other = findByMessageId(repo, "m2");
    BOOST_REQUIRE(other.has_value());
    BOOST_TEST(other->queueErn == kDlq);
    BOOST_TEST(other->sourceQueueErn == kInvoices);
}

BOOST_AUTO_TEST_CASE(AMessageWithNoRecordedOriginIsLeftWhereItIs) {
    MemoryEqsRepository repo;
    addQueue(repo, kDlq);
    addQueue(repo, kOrders, kDlq);
    addQueue(repo, kInvoices, kDlq);
    addDeadMessage(repo, "m1", "");

    // Two queues share the dead letter queue, so each is redriven by source. A message that
    // predates euclid recording its origin belongs to neither pass and stays put, rather than
    // being guessed into one of them.
    BOOST_TEST(repo.redriveMessages(kDlq, kOrders, kOrders) == 0L);
    BOOST_TEST(repo.redriveMessages(kDlq, kInvoices, kInvoices) == 0L);

    const auto stranded = findByMessageId(repo, "m1");
    BOOST_REQUIRE(stranded.has_value());
    BOOST_TEST(stranded->queueErn == kDlq);
}

BOOST_AUTO_TEST_CASE(RedriveIgnoresMessagesInOtherQueues) {
    MemoryEqsRepository repo;
    addQueue(repo, kDlq);
    addQueue(repo, kOrders, kDlq);

    Euclid::Database::Entity::EQS::Message live;
    live.messageId = "live";
    live.ern = "ern:eqs:eu-central-1:000000000000:message:live";
    live.queueErn = kOrders;
    live.body = "{}";
    repo.upsertMessage(live);

    BOOST_TEST(repo.redriveMessages(kDlq, kOrders, "") == 0L);
}
