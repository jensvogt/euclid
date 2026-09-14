// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EnsPublishPriorityTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/entity/eqs/MessagePriority.h>
#include <euclid/dto/ens/PublishMessageRequest.h>

using Euclid::Dto::ENS::PublishMessageRequest;
using Euclid::Database::Entity::EQS::MessagePriority;
using Euclid::Database::Entity::EQS::MessagePriorityFromString;
using Euclid::Database::Entity::EQS::MessagePriorityNames;
using Euclid::Database::Entity::EQS::TryMessagePriorityFromString;
using Euclid::Database::Entity::EQS::MessagePriorityToString;

// A topic is not consumed from, so a priority means nothing on the topic message itself. It is
// carried on publish-message so that the queue messages the topic's SQS-type subscriptions fan out
// to are worth what the message that caused them was worth - without it, a delivery that crosses a
// topic arrives on the other side at MEDIUM whatever it was sent as.
//
// Which makes the default the part worth pinning: publish-message is the one action here that
// predates the field, and a request from a client that has never heard of it has to keep meaning
// what it always meant rather than becoming a request for no priority at all.

BOOST_AUTO_TEST_CASE(PriorityRoundTrips) {

    const auto request = PublishMessageRequest::fromJson(R"({"ern":"topic-ern","body":"hello","attributes":{},"priority":"LOW"})");

    BOOST_CHECK_EQUAL(request.priority, "LOW");
    BOOST_CHECK_EQUAL(MessagePriorityToString(MessagePriorityFromString(request.priority)), "LOW");
}

BOOST_AUTO_TEST_CASE(RequestWithoutPriorityIsMiddle) {

    // What an older client sends: no priority field at all.
    const auto request = PublishMessageRequest::fromJson(R"({"ern":"topic-ern","body":"hello","attributes":{}})");

    BOOST_CHECK_EQUAL(request.priority, "MEDIUM");
    BOOST_CHECK_EQUAL(MessagePriorityToString(MessagePriorityFromString(request.priority)), "MEDIUM");
}

BOOST_AUTO_TEST_CASE(EmptyPriorityIsMiddleRatherThanNothing) {

    // And what a client that sends the field but leaves it blank means, which is the same thing -
    // not a priority of "", which would be neither LOW, MEDIUM nor HIGH.
    const auto request = PublishMessageRequest::fromJson(R"({"ern":"topic-ern","body":"hello","attributes":{},"priority":""})");

    BOOST_CHECK_EQUAL(request.priority, "MEDIUM");
}

BOOST_AUTO_TEST_CASE(SerializedRequestAlwaysCarriesAPriority) {

    // Written out even when it was never set, so the wire always says what was meant and the
    // server is never left inferring it from an absence.
    PublishMessageRequest request;
    request.ern = "topic-ern";
    request.body = "hello";

    BOOST_CHECK(request.toJson().find(R"("priority":"MEDIUM")") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(UnknownPriorityFallsBackToMedium) {

    // A value read back from storage, or one riding on a delivery in flight, has to land somewhere
    // sensible rather than at the bottom of the queue, which is where LOW would put it - failing
    // instead would cost a message somebody is waiting for.
    BOOST_CHECK_EQUAL(MessagePriorityToString(MessagePriorityFromString("URGENT")), "MEDIUM");
}

// The middle tier was called MIDDLE until 2026-09-14. The name changed; the value did not.
BOOST_AUTO_TEST_CASE(TheOldNameForTheMiddleTierIsStillRead) {

    // Every message stored before the rename carries it, and so does every request from a client
    // built against an older SDK - euclid's SDKs are released separately, so an installation is
    // routinely a version ahead of them.
    //
    // It resolves rather than falling through to the default. Those happen to be the same tier, so
    // a fallback would look right and be right by accident: the moment the default moved, every
    // stored MIDDLE row would quietly move with it.
    const auto legacy = TryMessagePriorityFromString("MIDDLE");
    BOOST_REQUIRE(legacy.has_value());
    BOOST_CHECK(legacy.value() == MessagePriority::MEDIUM);

    // Case-insensitive like the rest of them.
    BOOST_CHECK(TryMessagePriorityFromString("middle").has_value());
    BOOST_CHECK_EQUAL(MessagePriorityToString(MessagePriorityFromString("MIDDLE")), "MEDIUM");
}

BOOST_AUTO_TEST_CASE(NothingWritesTheOldNameAnyMore) {

    // Read-only: the old spelling is accepted and never produced, so it leaves the installation as
    // the rows carrying it are replaced rather than being written afresh for ever.
    for (const auto &[priority, name]: MessagePriorityNames) {
        BOOST_CHECK(name != "MIDDLE");
    }
    BOOST_CHECK_EQUAL(MessagePriorityToString(MessagePriority::MEDIUM), "MEDIUM");
}

BOOST_AUTO_TEST_CASE(AnUnknownPriorityOnARequestIsRefusedRatherThanAbsorbed) {

    // The same value on the way in is a caller's typo, and absorbing it means every send after it
    // is wrong in a way nothing reports: they asked for one priority and silently got another.
    // publish-message and send-message both refuse it; only the readings above are lenient.
    BOOST_CHECK(!TryMessagePriorityFromString("URGENT").has_value());
    BOOST_CHECK(!TryMessagePriorityFromString("").has_value());
}

BOOST_AUTO_TEST_CASE(PriorityIsReadWhateverItsCase) {

    // "low" from a person and "LOW" as it is stored mean the same thing; refusing one of them
    // teaches nobody anything, and is the shape of typo most likely to be made.
    BOOST_CHECK(TryMessagePriorityFromString("low") == Euclid::Database::Entity::EQS::MessagePriority::LOW);
    BOOST_CHECK(TryMessagePriorityFromString("Low") == Euclid::Database::Entity::EQS::MessagePriority::LOW);
    BOOST_CHECK(TryMessagePriorityFromString("HIGH") == Euclid::Database::Entity::EQS::MessagePriority::HIGH);
}
