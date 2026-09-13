// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE QueueOwnershipTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/entity/eqs/Queue.h>

using Euclid::Database::Entity::EQS::Queue;

// delete-queue and purge-queue were not resource-checked at all until 2026-09-13: a principal
// granted one queue by `eap create-application --queues` could remove any queue in its namespace.
// They are checked now, with one exception - a caller's own internal queue.
//
// The exception exists because an application does not consume from a topic, it consumes from a
// queue of its own that it subscribes to one, and that queue's name is generated on each start. It
// can therefore never appear in the resource list a deployment writes, so requiring a grant for it
// would mean no application could ever clean up after itself - including the queues a run that was
// killed rather than stopped left behind.
//
// The rule itself lives on the entity - Queue::isInternalPlumbingOf() - rather than inside the
// module, so that what the handlers depend on is the thing tested here and not a copy of it.

namespace {

    constexpr auto kApplication = "app-order-service";
    constexpr auto kOtherApplication = "app-invoice-service";

    Queue queueOf(const bool internal, const std::string &owner) {
        Queue queue;
        queue.name = internal ? "euclid-delivery-7c1f2f5e" : "orders";
        queue.ern = "ern:eqs:eu-central-1:000000000000:production:queue:" + queue.name;
        queue.accountId = "000000000000";
        queue.nameSpace = "production";
        queue.internal = internal;
        queue.owner = owner;
        return queue;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AnApplicationMayRemoveItsOwnDeliveryQueue) {

    // The whole point of the exception: this queue's name was invented at startup, so no --queues
    // list could have named it, and the listener container deletes it on shutdown.
    BOOST_TEST(queueOf(true, kApplication).isInternalPlumbingOf(kApplication));
}

BOOST_AUTO_TEST_CASE(ADeployedQueueIsNotExemptEvenFromItsOwnCreator) {

    // Creating a queue is not being granted it. An application that made "orders" still has to
    // hold it in its resource list to delete it - otherwise the list bounds sending and receiving
    // and not the one operation that loses the data.
    BOOST_TEST(!queueOf(false, kApplication).isInternalPlumbingOf(kApplication));
}

BOOST_AUTO_TEST_CASE(OneApplicationMayNotRemoveAnothersDeliveryQueue) {

    // Without the owner half, any application could stop any other from receiving - internal
    // queues are hidden from listings but a known ERN is enough to name one.
    BOOST_TEST(!queueOf(true, kOtherApplication).isInternalPlumbingOf(kApplication));
}

BOOST_AUTO_TEST_CASE(AnAnonymousCallerIsNotExempt) {

    // EqsServer answers the "queue is not there" and "caller is not authenticated" cases before it
    // asks this, but an empty user id must not match a queue whose owner was never recorded - a
    // queue written before owners existed would otherwise be everyone's plumbing.
    auto ownerless = queueOf(true, kApplication);
    ownerless.owner = "";

    BOOST_TEST(!ownerless.isInternalPlumbingOf(""));
    BOOST_TEST(!queueOf(true, kApplication).isInternalPlumbingOf(""));
}

BOOST_AUTO_TEST_CASE(BothHalvesAreRequired) {

    // Stated as one case as well, because each half on its own looks sufficient and is not.
    const auto ownAndInternal = queueOf(true, kApplication);

    auto internalOnly = ownAndInternal;
    internalOnly.owner = kOtherApplication;

    auto ownOnly = ownAndInternal;
    ownOnly.internal = false;

    BOOST_TEST(ownAndInternal.isInternalPlumbingOf(kApplication));
    BOOST_TEST(!internalOnly.isInternalPlumbingOf(kApplication));
    BOOST_TEST(!ownOnly.isInternalPlumbingOf(kApplication));
}
