// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE MaxMessageLengthTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/database/entity/ens/Topic.h>
#include <euclid/database/entity/eqs/Queue.h>
#include <euclid/dto/ens/CreateTopicRequest.h>
#include <euclid/dto/ens/SetTopicMaxMessageLengthRequest.h>
#include <euclid/dto/eqs/CreateQueueRequest.h>

using Euclid::Dto::ENS::CreateTopicRequest;
using Euclid::Dto::ENS::SetTopicMaxMessageLengthRequest;
using Euclid::Dto::EQS::CreateQueueRequest;

// maxMessageLength was recorded and reported for a long time before anything measured a message
// against it. Now publish-message and send-message do, which makes two things load-bearing that
// were previously free: what an absent field on a create request means, and what a stored zero
// means. Read the wrong way round, either one turns every publish to an existing topic into a 400.

namespace ENS = Euclid::Database::Entity::ENS;
namespace EQS = Euclid::Database::Entity::EQS;

// ── What "unset" resolves to ────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AStoredZeroMeansTheDefaultRatherThanNothing) {

    // The upgrade case: a topic created by a client that never sent the field holds zero. Read
    // literally it would accept no message at all.
    BOOST_TEST(ENS::EffectiveMaxMessageLength(0) == ENS::kDefaultMaxMessageLength);
    BOOST_TEST(EQS::EffectiveMaxMessageLength(0) == EQS::kDefaultMaxMessageLength);
}

BOOST_AUTO_TEST_CASE(AStoredNegativeAlsoMeansTheDefault) {

    // Nothing writes one - set-topic-max-message-length refuses it - but a hand-edited document or
    // an import can, and refusing every publish is the wrong way to react to it.
    BOOST_TEST(ENS::EffectiveMaxMessageLength(-1) == ENS::kDefaultMaxMessageLength);
    BOOST_TEST(EQS::EffectiveMaxMessageLength(-4096) == EQS::kDefaultMaxMessageLength);
}

BOOST_AUTO_TEST_CASE(AStoredLimitIsUsedAsGiven) {

    BOOST_TEST(ENS::EffectiveMaxMessageLength(4 * 1024 * 1024) == 4 * 1024 * 1024);
    BOOST_TEST(EQS::EffectiveMaxMessageLength(256) == 256);
}

BOOST_AUTO_TEST_CASE(TheDefaultIsOneMebibyte) {

    // The number itself, because it is what the documentation and every shipped default promise,
    // and because ENS and EQS have to agree on it.
    BOOST_TEST(ENS::kDefaultMaxMessageLength == 1024 * 1024);
    BOOST_TEST(EQS::kDefaultMaxMessageLength == 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(AFreshEntityCarriesTheDefault) {

    BOOST_TEST(ENS::Topic{}.maxMessageLength == ENS::kDefaultMaxMessageLength);
    BOOST_TEST(EQS::Queue{}.maxMessageLength == EQS::kDefaultMaxMessageLength);
}

// ── What a create request without the field means ───────────────────────────

BOOST_AUTO_TEST_CASE(CreateTopicWithoutTheFieldGetsTheDefault) {

    // What every SDK sends today: a name and nothing else. Read as zero, this would create a topic
    // that refuses everything published to it.
    const auto request = CreateTopicRequest::fromJson(R"({"name":"order-events"})");

    BOOST_TEST(request.maxMessageLength == ENS::kDefaultMaxMessageLength);
}

BOOST_AUTO_TEST_CASE(CreateQueueWithoutTheFieldGetsTheDefault) {

    const auto request = CreateQueueRequest::fromJson(R"({"name":"orders"})");

    BOOST_TEST(request.maxMessageLength == EQS::kDefaultMaxMessageLength);
}

BOOST_AUTO_TEST_CASE(CreateTopicHonoursAnExplicitLimit) {

    const auto request = CreateTopicRequest::fromJson(R"({"name":"order-events","maxMessageLength":2048})");

    BOOST_TEST(request.maxMessageLength == 2048L);
}

BOOST_AUTO_TEST_CASE(CreateQueueHonoursAnExplicitLimit) {

    const auto request = CreateQueueRequest::fromJson(R"({"name":"orders","maxMessageLength":2048})");

    BOOST_TEST(request.maxMessageLength == 2048L);
}

// ── The action that changes it ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(SetTopicMaxMessageLengthRoundTrips) {

    const auto request = SetTopicMaxMessageLengthRequest::fromJson(R"({"ern":"topic-ern","maxMessageLength":4194304})");

    BOOST_TEST(request.ern == "topic-ern");
    BOOST_TEST(request.maxMessageLength == 4194304L);
}

BOOST_AUTO_TEST_CASE(SetTopicMaxMessageLengthWithoutTheFieldIsRefusedByTheServer) {

    // Zero here is not "the default" but a missing argument, and the handler refuses it - which is
    // why this request does not default the way a create request does. The two read the same field
    // name and mean different things by an absent one: create says "the usual", set says nothing
    // at all.
    const auto request = SetTopicMaxMessageLengthRequest::fromJson(R"({"ern":"topic-ern"})");

    BOOST_TEST(request.maxMessageLength == 0L);
}
