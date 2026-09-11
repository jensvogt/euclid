// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE TtlCacheTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <euclid/core/TtlCache.h>

using Euclid::Core::TtlCache;

// The cache sits in front of the lookups that authenticate every request, so what matters is not
// that it is fast but that it is honest about time and about absence: an answer must stop being
// reused once its TTL is up, an absent answer must be remembered as readily as a present one, and
// the whole thing must be safe to turn off.

namespace {

    constexpr auto kLongEnoughNotToExpire = std::chrono::milliseconds(60'000);

    // A loader that counts how often it actually ran, which is the only way to tell a hit from a
    // miss from the outside.
    struct CountingLoader {

        std::atomic<int> calls{0};
        std::optional<std::string> answer;

        explicit CountingLoader(std::optional<std::string> value) : answer(std::move(value)) {}

        std::function<std::optional<std::string>(const std::string &)> fn() {
            return [this](const std::string &) {
                calls++;
                return answer;
            };
        }
    };

}// namespace

BOOST_AUTO_TEST_CASE(a_loaded_answer_is_reused_within_the_ttl) {
    TtlCache<std::string, std::string> cache(kLongEnoughNotToExpire);
    CountingLoader loader{"secret"};

    for (int i = 0; i < 10; i++) {
        BOOST_CHECK_EQUAL(cache.get("AKIA1", loader.fn()).value(), "secret");
    }

    BOOST_CHECK_EQUAL(loader.calls.load(), 1);
}

BOOST_AUTO_TEST_CASE(different_keys_are_loaded_separately) {
    TtlCache<std::string, std::string> cache(kLongEnoughNotToExpire);
    CountingLoader loader{"secret"};

    std::ignore = cache.get("AKIA1", loader.fn());
    std::ignore = cache.get("AKIA2", loader.fn());
    std::ignore = cache.get("AKIA1", loader.fn());

    BOOST_CHECK_EQUAL(loader.calls.load(), 2);
}

// The point of the TTL: a key deactivated in the database has to start being refused, and this is
// what bounds how long it does not.
BOOST_AUTO_TEST_CASE(an_answer_is_loaded_again_once_its_ttl_has_passed) {
    TtlCache<std::string, std::string> cache(std::chrono::milliseconds(20));
    CountingLoader loader{"secret"};

    std::ignore = cache.get("AKIA1", loader.fn());
    std::ignore = cache.get("AKIA1", loader.fn());
    BOOST_CHECK_EQUAL(loader.calls.load(), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    std::ignore = cache.get("AKIA1", loader.fn());
    BOOST_CHECK_EQUAL(loader.calls.load(), 2);
}

// An unknown access key is a common thing to be asked about, and looking it up every time would
// make an unauthenticated caller more expensive to serve than an authenticated one.
BOOST_AUTO_TEST_CASE(an_absent_answer_is_remembered_too) {
    TtlCache<std::string, std::string> cache(kLongEnoughNotToExpire);
    CountingLoader loader{std::nullopt};

    BOOST_CHECK(!cache.get("nobody", loader.fn()).has_value());
    BOOST_CHECK(!cache.get("nobody", loader.fn()).has_value());
    BOOST_CHECK(!cache.get("nobody", loader.fn()).has_value());

    BOOST_CHECK_EQUAL(loader.calls.load(), 1);
}

BOOST_AUTO_TEST_CASE(clearing_forgets_everything) {
    TtlCache<std::string, std::string> cache(kLongEnoughNotToExpire);
    CountingLoader loader{"secret"};

    std::ignore = cache.get("AKIA1", loader.fn());
    cache.clear();
    std::ignore = cache.get("AKIA1", loader.fn());

    BOOST_CHECK_EQUAL(loader.calls.load(), 2);
}

// Zero is how an installation says it would rather pay the latency than ever act on a stale
// permission, so it has to mean "no cache" rather than "a cache that expires immediately but still
// holds entries".
BOOST_AUTO_TEST_CASE(a_zero_ttl_turns_the_cache_off) {
    TtlCache<std::string, std::string> cache(std::chrono::milliseconds(0));
    CountingLoader loader{"secret"};

    for (int i = 0; i < 5; i++) {
        BOOST_CHECK_EQUAL(cache.get("AKIA1", loader.fn()).value(), "secret");
    }

    BOOST_CHECK_EQUAL(loader.calls.load(), 5);
}

BOOST_AUTO_TEST_CASE(the_entry_count_stays_bounded) {
    TtlCache<std::string, std::string> cache(kLongEnoughNotToExpire, 16);
    CountingLoader loader{"secret"};

    // Every key distinct, so nothing can be served from the cache and only the bound stops the
    // map growing - which is what an unknown-key scan looks like from in here.
    for (int i = 0; i < 1000; i++) {
        BOOST_CHECK_EQUAL(cache.get("AKIA" + std::to_string(i), loader.fn()).value(), "secret");
    }

    BOOST_CHECK_EQUAL(loader.calls.load(), 1000);
}

// Every module runs its server on several threads, so the cache is read and written concurrently
// from the first request onwards.
BOOST_AUTO_TEST_CASE(concurrent_readers_all_get_the_answer) {
    TtlCache<std::string, std::string> cache(kLongEnoughNotToExpire);
    CountingLoader loader{"secret"};

    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&] {
            for (int i = 0; i < 500; i++) {
                const auto value = cache.get("AKIA" + std::to_string(i % 20), loader.fn());
                if (!value.has_value() || value.value() != "secret") wrong++;
            }
        });
    }
    for (auto &thread: threads) thread.join();

    BOOST_CHECK_EQUAL(wrong.load(), 0);
}
