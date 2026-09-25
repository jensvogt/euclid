// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BucketPriorityTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/entity/eqs/MessagePriority.h>
#include <euclid/database/entity/esm/Bucket.h>
#include <euclid/dto/esm/CreateBucketRequest.h>

using Euclid::Database::Entity::ESM::Bucket;
using Euclid::Database::Entity::ESM::NotificationPriority;
using Euclid::Database::Entity::EQS::MessagePriorityToString;
using Euclid::Database::Entity::EQS::TryMessagePriorityFromString;

// A bucket's priority does nothing to the bucket. It exists to be handed to the messages a
// subscription of that bucket turns an object event into, and the only question with an answer worth
// pinning is which statement wins when two of them disagree.

BOOST_AUTO_TEST_SUITE(BucketPriorityTest)

    // ── the precedence ──────────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(TheNarrowerStatementWins) {

        // A bucket says "everything from here is urgent"; an object says "this one is". The object is
        // the narrower claim, so it wins - which is what lets a bucket set a floor without taking
        // away the ability to say more about a particular object.
        BOOST_TEST(NotificationPriority("HIGH", "LOW") == "HIGH");
        BOOST_TEST(NotificationPriority("LOW", "HIGH") == "LOW");
    }

    BOOST_AUTO_TEST_CASE(TheBucketAnswersWhenTheObjectDoesNot) {

        // The case the field exists for: nothing set the object's priority, and the bucket's is what
        // the notification should carry.
        BOOST_TEST(NotificationPriority("", "HIGH") == "HIGH");
    }

    BOOST_AUTO_TEST_CASE(NeitherMeansNeitherAndNotMedium) {

        // The distinction the whole design rests on. An empty answer leaves the target queue's own
        // default in force, which is what happened before a bucket could carry a priority at all.
        // Answering MEDIUM here would have every bucket silently override every queue that had
        // chosen something else - and a queue created with LOW would start delivering at MEDIUM
        // because somebody added a bucket subscription to it.
        BOOST_TEST(NotificationPriority("", "").empty());
    }

    // ── what may be stored ──────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(EveryPriorityAQueueAcceptsABucketAcceptsToo) {

        // The same vocabulary, because the value ends up on a message: a bucket that accepted a
        // priority EQS then refused would be a bucket whose notifications silently lost it.
        for (const auto *priority: {"LOW", "MEDIUM", "HIGH"}) {
            BOOST_TEST_CONTEXT(priority) {
                BOOST_TEST(TryMessagePriorityFromString(priority).has_value());
            }
        }

        // Case is not part of it - set-bucket-priority stores what TryMessagePriorityFromString
        // accepted, so "high" comes back as "HIGH" and reads the same as a queue's or a message's.
        const auto parsed = TryMessagePriorityFromString("high");
        BOOST_REQUIRE(parsed.has_value());
        BOOST_TEST(MessagePriorityToString(*parsed) == "HIGH");

        // And the older spelling of the middle tier is still read, so a bucket set by a client built
        // against an older SDK is not refused.
        BOOST_TEST(TryMessagePriorityFromString("MIDDLE").has_value());
    }

    BOOST_AUTO_TEST_CASE(ANonsensePriorityIsRefusedRatherThanStored) {

        // What create-bucket and set-bucket-priority check before writing. A bucket that had quietly
        // dropped one would look configured and behave as though it were not.
        BOOST_TEST(!TryMessagePriorityFromString("URGENT").has_value());
        BOOST_TEST(!TryMessagePriorityFromString("1").has_value());
    }

    // ── the entity and the wire ─────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(ABucketStartsWithNoPriority) {

        // Not MEDIUM. Every bucket that existed before this field did reads back empty, and behaves
        // exactly as it did.
        const Bucket bucket;
        BOOST_TEST(bucket.priority.empty());
    }

    BOOST_AUTO_TEST_CASE(ThePriorityRoundTripsThroughTheRequest) {

        const auto request = Euclid::Dto::ESM::CreateBucketRequest::fromJson(
                R"({"name": "inbox", "internal": false, "priority": "HIGH"})");
        BOOST_TEST(request.name == "inbox");
        BOOST_TEST(request.priority == "HIGH");

        // And a request that says nothing about it leaves it unset rather than defaulting.
        const auto silent = Euclid::Dto::ESM::CreateBucketRequest::fromJson(R"({"name": "inbox"})");
        BOOST_TEST(silent.priority.empty());
    }

BOOST_AUTO_TEST_SUITE_END()
