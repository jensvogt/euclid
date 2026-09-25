// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE LongPollSlotsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

// Euclid includes
#include <euclid/core/LongPollSlots.h>

using Euclid::Core::LongPollSlots;

// A long poll spends a worker thread doing nothing for up to twenty seconds, which is the right
// shape for "tell me when there is work" and the wrong shape for a small thread pool: enough
// clients waiting and there is no thread left to answer anyone who is not waiting. What that looks
// like from outside is not an error - the request queues, and is served the moment a wait ends,
// which is often just after the caller timed out and gave up on it.
//
// So the one thing that has to hold is that waiting can never consume every thread.

BOOST_AUTO_TEST_CASE(WaitsAreAllowedUpToTheLimit) {
    LongPollSlots slots;
    slots.limit(3);

    const auto first = slots.acquire();
    const auto second = slots.acquire();
    const auto third = slots.acquire();

    BOOST_TEST(first.held());
    BOOST_TEST(second.held());
    BOOST_TEST(third.held());
}

BOOST_AUTO_TEST_CASE(TheThreadAfterTheLimitDoesNotWait) {
    LongPollSlots slots;
    slots.limit(2);

    const auto first = slots.acquire();
    const auto second = slots.acquire();
    const auto third = slots.acquire();

    BOOST_TEST(first.held());
    BOOST_TEST(second.held());
    // Not a failure: this request answers with whatever is there instead of waiting, which is
    // what a long poll returns anyway when nothing arrives.
    BOOST_TEST(!third.held());
}

BOOST_AUTO_TEST_CASE(ASlotIsGivenBackWhenTheRequestEnds) {
    LongPollSlots slots;
    slots.limit(1);

    {
        const auto held = slots.acquire();
        BOOST_TEST(held.held());
        BOOST_TEST(!slots.acquire().held());
    }

    // The handler returned, however it returned - the count goes back down with it.
    BOOST_TEST(slots.acquire().held());
}

BOOST_AUTO_TEST_CASE(ARefusedSlotDoesNotConsumeOne) {
    LongPollSlots slots;
    slots.limit(1);

    const auto held = slots.acquire();
    {
        const auto refused = slots.acquire();
        BOOST_TEST(!refused.held());
    }

    // If a refusal had counted, releasing it would have left the counter below zero and handed
    // out a second slot here.
    BOOST_TEST(!slots.acquire().held());
}

BOOST_AUTO_TEST_CASE(ASingleThreadedModuleCanStillWait) {
    // threads - 1 is zero for a one-thread module. Refusing every wait there would be pointless:
    // one request is all it can serve at a time either way, so there is no second request to
    // protect.
    LongPollSlots slots;
    slots.limit(0);

    BOOST_TEST(slots.acquire().held());
}

BOOST_AUTO_TEST_CASE(NoMoreThanTheLimitEverWaitAtOnce) {
    // The property that matters, under the concurrency it exists for: whatever the interleaving,
    // the number of threads waiting at any instant never exceeds the cap - so a module with more
    // threads than the cap always has one left for a request that is not waiting.
    constexpr int kLimit = 4;
    constexpr int kThreads = 32;

    LongPollSlots slots;
    slots.limit(kLimit);

    std::atomic<int> waiting{0};
    std::atomic<int> peak{0};
    std::atomic<int> refused{0};

    // Every thread attempts at the same instant, and whoever gets a slot holds it until all of them
    // have tried. Without that this was a race the test happened to win: the body of one attempt is
    // a few atomic operations, so a thread could run all two hundred of them before the next thread
    // was even created - 32 threads then contended with nobody, the limit was never reached, and
    // "some had to be turned away" was a coin toss decided by how fast the platform starts threads.
    // On Windows, where starting one costs far more than the work it was about to do, the coin came
    // down the other way every time.
    std::latch ready(kThreads);
    std::atomic<int> attempted{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            ready.arrive_and_wait();

            const auto slot = slots.acquire();
            if (!slot.held()) {
                refused.fetch_add(1);
                attempted.fetch_add(1);
                return;
            }

            const int now = waiting.fetch_add(1) + 1;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {}

            // Held until the last thread has had its answer, which is what makes the contention
            // real rather than hoped for. Every thread increments before anybody waits on it, so
            // this always ends.
            attempted.fetch_add(1);
            while (attempted.load() < kThreads) std::this_thread::yield();

            waiting.fetch_sub(1);
        });
    }
    for (auto &thread: threads) thread.join();

    BOOST_TEST(peak.load() <= kLimit);
    BOOST_TEST(waiting.load() == 0);

    // Exactly, not merely "some": 32 threads asked at once for 4 slots, so 28 of them were turned
    // away. A test that only asked for more than zero could pass having exercised the limit once.
    BOOST_TEST(refused.load() == kThreads - kLimit);
}

// The same property under churn rather than under one simultaneous rush: slots taken and given back
// as fast as the threads can manage, which is what a module under load actually does. Nothing is
// asserted about refusals here - whether any two of these overlap is the platform's business - only
// that the cap holds and nothing is left behind.
BOOST_AUTO_TEST_CASE(SlotsAreGivenBackHoweverTheyAreTakenAndReturned) {
    constexpr int kLimit = 4;
    constexpr int kThreads = 16;

    LongPollSlots slots;
    slots.limit(kLimit);

    std::atomic<int> waiting{0};
    std::atomic<int> peak{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int attempt = 0; attempt < 200; ++attempt) {
                const auto slot = slots.acquire();
                if (!slot.held()) continue;

                const int now = waiting.fetch_add(1) + 1;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
                std::this_thread::yield();
                waiting.fetch_sub(1);
            }
        });
    }
    for (auto &thread: threads) thread.join();

    BOOST_TEST(peak.load() <= kLimit);
    BOOST_TEST(waiting.load() == 0);

    // And the slots are all back - all four at once, which is the whole assertion: acquiring one
    // four times in a row would pass with three of them leaked, since each temporary gives its slot
    // back at the end of its own statement. A leak here is a module that answers fewer long polls
    // after every burst until it answers none.
    //
    // Named rather than collected: Slot has a deleted copy and no move, so it cannot go in a
    // container - it is meant to live exactly as long as the scope that took it.
    const auto first = slots.acquire();
    const auto second = slots.acquire();
    const auto third = slots.acquire();
    const auto fourth = slots.acquire();

    BOOST_TEST(first.held());
    BOOST_TEST(second.held());
    BOOST_TEST(third.held());
    BOOST_TEST(fourth.held());
    static_assert(kLimit == 4, "one named slot per limit, so that all of them are held at once");
}
