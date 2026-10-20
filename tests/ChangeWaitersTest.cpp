// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ChangeWaitersTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <thread>

// Euclid includes
#include <euclid/database/ChangeWaiters.h>

using Euclid::Database::ChangeWaiters;
using namespace std::chrono_literals;

// The wake-up half of a long poll, with no database behind it: Watch() is never called, so these
// waiters run at their polled fallback and are woken only by Wake(). What is tested is what makes
// the long polls correct rather than merely fast - a wake-up is never lost, a wait never outlives
// its deadline or its fallback, and one key's wake-up is not another's.

namespace {

    using Clock = std::chrono::steady_clock;

    // A fallback long enough that a wait which ends early can only have been woken.
    ChangeWaiters slowFallback() { return {"test", 10s, 10s}; }

}// namespace

BOOST_AUTO_TEST_CASE(AWakeFromAnotherThreadEndsTheWaitEarly) {
    auto waiters = slowFallback();
    const auto waiter = waiters.Enroll("queue-a");
    const auto seen = waiter.Generation();

    std::thread waker([&] {
        std::this_thread::sleep_for(100ms);
        waiters.Wake("queue-a");
    });

    const auto started = Clock::now();
    waiter.Wait(seen, started + 5s);
    const auto took = Clock::now() - started;
    waker.join();

    BOOST_TEST(took < 2s);
    BOOST_TEST(waiter.Generation() != seen);
}

BOOST_AUTO_TEST_CASE(AWakeBetweenLookingAndWaitingIsNotLost) {
    auto waiters = slowFallback();
    const auto waiter = waiters.Enroll("queue-a");

    // The race the generation exists for: the look found nothing, the message arrived, and only
    // then does the wait start. It must return at once rather than sleep through the message.
    const auto seen = waiter.Generation();
    waiters.Wake("queue-a");

    const auto started = Clock::now();
    waiter.Wait(seen, started + 5s);

    BOOST_TEST((Clock::now() - started) < 1s);
}

BOOST_AUTO_TEST_CASE(AnotherKeysWakeDoesNotEndTheWait) {
    auto waiters = slowFallback();
    const auto waiter = waiters.Enroll("queue-a");
    const auto seen = waiter.Generation();

    waiters.Wake("queue-b");

    const auto started = Clock::now();
    waiter.Wait(seen, started + 300ms);

    BOOST_TEST((Clock::now() - started) >= 300ms);
    BOOST_TEST(waiter.Generation() == seen);
}

BOOST_AUTO_TEST_CASE(NothingToWakeItAnswersAtTheDeadline) {
    auto waiters = slowFallback();
    const auto waiter = waiters.Enroll("queue-a");

    const auto started = Clock::now();
    waiter.Wait(waiter.Generation(), started + 300ms);
    const auto took = Clock::now() - started;

    BOOST_TEST(took >= 300ms);
    BOOST_TEST(took < 3s);
}

BOOST_AUTO_TEST_CASE(TheFallbackCapsAWaitWithAFarDeadline) {
    // Without a change stream the fallback is the poll interval, so a wait has to come back at it
    // even with nothing woken and the deadline far off - otherwise a message nobody reports, such
    // as one whose delay ran out, would wait for the whole of the long poll.
    ChangeWaiters waiters{"test", 10s, 200ms};
    const auto waiter = waiters.Enroll("queue-a");

    const auto started = Clock::now();
    waiter.Wait(waiter.Generation(), started + 10s);
    const auto took = Clock::now() - started;

    BOOST_TEST(took >= 200ms);
    BOOST_TEST(took < 3s);
}

BOOST_AUTO_TEST_CASE(EveryWaiterOnAKeyIsWoken) {
    auto waiters = slowFallback();
    const auto first = waiters.Enroll("queue-a");
    const auto second = waiters.Enroll("queue-a");
    const auto firstSeen = first.Generation();
    const auto secondSeen = second.Generation();

    std::thread waker([&] {
        std::this_thread::sleep_for(100ms);
        waiters.Wake("queue-a");
    });

    const auto started = Clock::now();
    std::thread other([&] { second.Wait(secondSeen, started + 5s); });
    first.Wait(firstSeen, started + 5s);
    other.join();
    waker.join();

    BOOST_TEST((Clock::now() - started) < 2s);
}

BOOST_AUTO_TEST_CASE(AKeyNobodyWaitsOnStartsAfresh) {
    auto waiters = slowFallback();

    // Woken while nobody waits: nothing is kept for it, so a waiter enrolled afterwards is not
    // handed a wake-up that was meant for nobody - it looks for itself, as it does anyway.
    waiters.Wake("queue-a");
    {
        const auto waiter = waiters.Enroll("queue-a");
        BOOST_TEST(waiter.Generation() == 0U);
        waiters.Wake("queue-a");
        BOOST_TEST(waiter.Generation() == 1U);
    }

    const auto again = waiters.Enroll("queue-a");
    BOOST_TEST(again.Generation() == 0U);
}
