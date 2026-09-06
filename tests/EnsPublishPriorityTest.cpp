#define BOOST_TEST_MODULE EnsPublishPriorityTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/entity/eqs/MessagePriority.h>
#include <euclid/dto/ens/PublishMessageRequest.h>

using Euclid::Dto::ENS::PublishMessageRequest;
using Euclid::Database::Entity::EQS::MessagePriorityFromString;
using Euclid::Database::Entity::EQS::MessagePriorityToString;

// A topic is not consumed from, so a priority means nothing on the topic message itself. It is
// carried on publish-message so that the queue messages the topic's SQS-type subscriptions fan out
// to are worth what the message that caused them was worth - without it, a delivery that crosses a
// topic arrives on the other side at MIDDLE whatever it was sent as.
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

    BOOST_CHECK_EQUAL(request.priority, "MIDDLE");
    BOOST_CHECK_EQUAL(MessagePriorityToString(MessagePriorityFromString(request.priority)), "MIDDLE");
}

BOOST_AUTO_TEST_CASE(EmptyPriorityIsMiddleRatherThanNothing) {

    // And what a client that sends the field but leaves it blank means, which is the same thing -
    // not a priority of "", which would be neither LOW, MIDDLE nor HIGH.
    const auto request = PublishMessageRequest::fromJson(R"({"ern":"topic-ern","body":"hello","attributes":{},"priority":""})");

    BOOST_CHECK_EQUAL(request.priority, "MIDDLE");
}

BOOST_AUTO_TEST_CASE(SerializedRequestAlwaysCarriesAPriority) {

    // Written out even when it was never set, so the wire always says what was meant and the
    // server is never left inferring it from an absence.
    PublishMessageRequest request;
    request.ern = "topic-ern";
    request.body = "hello";

    BOOST_CHECK(request.toJson().find(R"("priority":"MIDDLE")") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(UnknownPriorityFallsBackToMiddle) {

    // The server reads the string rather than validating it, so a typo has to land somewhere
    // sensible instead of dropping the message to the bottom of the queue.
    BOOST_CHECK_EQUAL(MessagePriorityToString(MessagePriorityFromString("URGENT")), "MIDDLE");
}
