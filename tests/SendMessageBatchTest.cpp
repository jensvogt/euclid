// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE SendMessageBatchTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eqs/MessageBatch.h>

using Euclid::Database::Entity::EQS::BatchEntryRefusal;
using Euclid::Database::Entity::EQS::BatchRefusal;

// send-message-batch draws a line other multi-item actions in euclid do not. esm delete-objects skips a key
// that names nothing and only counts what went, because a delete of something already gone is the
// outcome the caller asked for. A send is not like that: a message that does not go is a message
// dropped, and a producer holding "97 of 100" with no way to learn which three would have to send
// all hundred again and duplicate ninety-seven.
//
// So two kinds of refusal, and keeping them apart is the whole of what these cover. A request whose
// *shape* is wrong fails outright; a *message* that cannot be sent is reported against its index and
// the rest still go.

BOOST_AUTO_TEST_CASE(AnEmptyBatchIsRefusedRatherThanAnsweredWithZero) {

    // "Sent nothing" would be a 200 to every caller that checks a status code and moves on, and the
    // caller meant to send something.
    BOOST_TEST(!BatchRefusal(0, 100).empty());
    BOOST_TEST(!BatchRefusal(-1, 100).empty());
}

BOOST_AUTO_TEST_CASE(ABatchOverTheCapIsRefusedAndSaysWhatTheCapIs) {

    BOOST_TEST(BatchRefusal(100, 100).empty());
    BOOST_TEST(BatchRefusal(1, 100).empty());

    const auto refusal = BatchRefusal(101, 100);
    BOOST_TEST(!refusal.empty());

    // Both numbers, because "too many" without either is a message somebody has to go and look up
    // the answer to - and the setting's name, so they know where to change it.
    BOOST_TEST(refusal.find("101") != std::string::npos);
    BOOST_TEST(refusal.find("100") != std::string::npos);
    BOOST_TEST(refusal.find("euclid.modules.eqs.max-batch-size") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheCapIsInclusive) {

    // Exactly at the cap goes; one past it does not. Stated on its own because an off-by-one here
    // is invisible until somebody sends precisely the number they were told was allowed.
    BOOST_TEST(BatchRefusal(100, 100).empty());
    BOOST_TEST(!BatchRefusal(101, 100).empty());
    BOOST_TEST(BatchRefusal(1, 1).empty());
    BOOST_TEST(!BatchRefusal(2, 1).empty());
}

BOOST_AUTO_TEST_CASE(AMessageOverTheQueuesLimitIsRefusedOnItsOwn) {

    constexpr long kMax = 1024;

    BOOST_TEST(BatchEntryRefusal(1024, kMax, "").empty());
    BOOST_TEST(BatchEntryRefusal(0, kMax, "").empty());

    const auto refusal = BatchEntryRefusal(2048, kMax, "");
    BOOST_TEST(!refusal.empty());

    // The same words a single send uses, so a caller moving from send-message to send-message-batch is not
    // asked to learn a second vocabulary for the same rejection.
    BOOST_TEST(refusal.find("2048") != std::string::npos);
    BOOST_TEST(refusal.find("1024") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnUnreadablePriorityIsRefusedRatherThanDefaulted) {

    // Absorbing an unreadable one would be worse than refusing it: a caller who wrote "URGENT" and
    // silently got the queue default is wrong on every send after that in the same invisible way.
    BOOST_TEST(!BatchEntryRefusal(10, 1024, "URGENT").empty());
    BOOST_TEST(!BatchEntryRefusal(10, 1024, "highest").empty());

    BOOST_TEST(BatchEntryRefusal(10, 1024, "LOW").empty());
    BOOST_TEST(BatchEntryRefusal(10, 1024, "MEDIUM").empty());
    BOOST_TEST(BatchEntryRefusal(10, 1024, "HIGH").empty());
}

BOOST_AUTO_TEST_CASE(PriorityIsReadTheSameWayASingleSendReadsIt) {

    // TryMessagePriorityFromString uppercases before it looks, so case is not part of the name -
    // and it resolves the legacy MIDDLE spelling to MEDIUM. Both matter here only because a batch
    // must accept exactly what send-message accepts: a message that can be sent on its own and not
    // in a batch is the worse of the two ways to disagree, since there is then no way to send it.
    BOOST_TEST(BatchEntryRefusal(10, 1024, "low").empty());
    BOOST_TEST(BatchEntryRefusal(10, 1024, "High").empty());
    BOOST_TEST(BatchEntryRefusal(10, 1024, "mEdIuM").empty());
    BOOST_TEST(BatchEntryRefusal(10, 1024, "MIDDLE").empty());
}

BOOST_AUTO_TEST_CASE(AnEmptyPriorityMeansTheQueuesOwnAndIsNotARefusal) {

    // Distinct from an unreadable one. send-message-batch leaves an unset priority empty rather than
    // defaulting it to MEDIUM in the DTO, so that the queue's own default still applies - which it
    // would not if the parse layer had already turned every unset entry into MEDIUM.
    BOOST_TEST(BatchEntryRefusal(10, 1024, "").empty());
}

BOOST_AUTO_TEST_CASE(OneBadMessageDoesNotCostTheGoodOnesAndKeepsItsIndex) {

    // The property the whole design turns on, walked the way the handler walks it: entry 1 is too
    // long, entry 3 has an unreadable priority, and the other three are sent. What the caller needs
    // back is which two, by position in the list it sent.
    constexpr long kMax = 100;
    const std::vector<std::pair<long, std::string> > entries{
            {10, "HIGH"},   // 0 - fine
            {500, ""},      // 1 - too long
            {10, ""},       // 2 - fine, takes the queue's priority
            {10, "urgent"}, // 3 - unreadable priority
            {100, "LOW"},   // 4 - exactly at the limit, fine
    };

    std::vector<long> sent;
    std::vector<long> failedIndexes;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto &[size, priority] = entries[i];
        if (BatchEntryRefusal(size, kMax, priority).empty()) {
            sent.push_back(static_cast<long>(i));
        } else {
            failedIndexes.push_back(static_cast<long>(i));
        }
    }

    BOOST_REQUIRE(sent.size() == 3U);
    BOOST_TEST(sent[0] == 0);
    BOOST_TEST(sent[1] == 2);
    BOOST_TEST(sent[2] == 4);

    BOOST_REQUIRE(failedIndexes.size() == 2U);
    BOOST_TEST(failedIndexes[0] == 1);
    BOOST_TEST(failedIndexes[1] == 3);

    // asked == sent + failed, which is what makes the response's three numbers consistent. A
    // caller checking this is entitled to rely on it.
    BOOST_TEST(sent.size() + failedIndexes.size() == entries.size());
}

BOOST_AUTO_TEST_CASE(EveryMessageBeingBadIsStillAnAnsweredRequest) {

    // Nothing to send is not the same as a bad request. Each message has its own reason, and the
    // caller has exactly what it needs to fix them - refusing the request would take those away.
    constexpr long kMax = 10;
    for (const auto size: {11L, 12L, 100L}) {
        BOOST_TEST(!BatchEntryRefusal(size, kMax, "").empty());
    }

    // The batch itself is still well formed: three messages is a legitimate request.
    BOOST_TEST(BatchRefusal(3, 100).empty());
}
