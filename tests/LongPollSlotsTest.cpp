#define BOOST_TEST_MODULE LongPollSlotsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
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

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int attempt = 0; attempt < 200; ++attempt) {
                const auto slot = slots.acquire();
                if (!slot.held()) {
                    refused.fetch_add(1);
                    continue;
                }
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
    // With 32 threads contending for 4 slots, some had to be turned away - otherwise this test
    // would be passing without ever exercising the limit.
    BOOST_TEST(refused.load() > 0);
}
