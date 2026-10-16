// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EgressStoreTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <EgressStore.h>
#include <euclid/database/Database.h>

using namespace Euclid::EAP::Egress;
using Euclid::Database::Database;

// The permits only mean anything if every EAP instance decides on the same state. Here the
// instances are threads, each doing what an EAP process does - read the site, decide, write back
// if nobody else did - against one store. What must hold is what a site would see: never more
// requests in flight than the limit, however many ask at once.

namespace {

    void freshDatabase() {
        Database::instance().initializeMemory();
    }

    // The application the state belongs to, by ERN - what the handlers key it by.
    constexpr auto kDownloader = "ern:eap:eu-central-1:000000000000:application/default/downloader";
    constexpr auto kIndexer = "ern:eap:eu-central-1:000000000000:application/default/indexer";

    // One EAP instance's acquire, as the handler does it.
    std::optional<std::string> tryAcquire(const std::string &site, const std::string &id, const std::string &application = kDownloader) {
        Decision decision;
        const bool written = updateSite(application, "app", site, [&](StoredSite &stored, const Clock::time_point now) {
            decision = Acquire(stored.state, limitsFor(stored), now, id, std::chrono::seconds{60});
            return true;
        });
        if (written && decision.granted) return decision.permit->id;
        return std::nullopt;
    }

    void release(const std::string &site, const std::string &id, const std::string &application = kDownloader) {
        BOOST_REQUIRE(updateSite(application, "app", site, [&](StoredSite &stored, const Clock::time_point) {
            if (stored.version < 0) return false;
            return Release(stored.state, id);
        }));
    }

    void setLimits(const std::string &site, const Limits &limits, const std::string &application = kDownloader) {
        BOOST_REQUIRE(updateSite(application, "app", site, [&](StoredSite &stored, const Clock::time_point) {
            stored.limits = limits;
            return true;
        }));
    }

}// namespace

BOOST_AUTO_TEST_CASE(ASiteIsCreatedOnFirstUseAndRemembered) {

    freshDatabase();
    setLimits("a.example.com", {.requestsPerSecond = 0, .burst = 1, .concurrent = 1});

    const auto first = tryAcquire("a.example.com", "one");
    BOOST_TEST(first.has_value());
    BOOST_TEST(!tryAcquire("a.example.com", "two").has_value());

    const auto stored = loadSite(egressCollection(), kDownloader, "a.example.com");
    BOOST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->state.permits.size() == 1U);
    BOOST_TEST(stored->limits->concurrent == 1L);
}

// Sixteen instances, each wanting slot after slot, on a site that takes four at a time. Each
// holds what it gets for a moment and gives it back. If two of them ever decided on the same read,
// both would see a free slot and the site would get five.
BOOST_AUTO_TEST_CASE(ManyInstancesTogetherNeverExceedTheConcurrency) {

    freshDatabase();
    const std::string site = "busy.example.com";
    constexpr long kConcurrent = 4;
    setLimits(site, {.requestsPerSecond = 0, .burst = 1, .concurrent = kConcurrent});

    std::atomic<long> inFlight{0};
    std::atomic<long> highest{0};
    std::atomic<long> granted{0};

    std::vector<std::thread> instances;
    for (int i = 0; i < 16; ++i) {
        instances.emplace_back([&, i] {
            for (int n = 0; n < 40; ++n) {
                const auto id = std::to_string(i) + "-" + std::to_string(n);
                if (!tryAcquire(site, id)) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    continue;
                }
                const auto now = ++inFlight;
                for (auto seen = highest.load(); now > seen && !highest.compare_exchange_weak(seen, now);) {}
                ++granted;
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                --inFlight;
                release(site, id);
            }
        });
    }
    for (auto &t: instances) t.join();

    BOOST_TEST(highest.load() <= kConcurrent);
    BOOST_TEST(granted.load() > 0L);
    BOOST_TEST(loadSite(egressCollection(), kDownloader, site)->state.permits.empty());
}

// The rate across instances: a burst of 5 and a rate too slow to matter within the test, so
// however many ask, exactly five get through.
BOOST_AUTO_TEST_CASE(ManyInstancesTogetherShareOneBucket) {

    freshDatabase();
    const std::string site = "slow.example.com";
    setLimits(site, {.requestsPerSecond = 0.001, .burst = 5, .concurrent = 0});

    std::atomic<long> granted{0};
    std::vector<std::thread> instances;
    for (int i = 0; i < 12; ++i) {
        instances.emplace_back([&, i] {
            for (int n = 0; n < 10; ++n) {
                if (tryAcquire(site, std::to_string(i) + "-" + std::to_string(n))) ++granted;
            }
        });
    }
    for (auto &t: instances) t.join();

    BOOST_TEST(granted.load() == 5L);
}

// Two applications calling the same site: each has its own allowance, so the one that has to go
// gently is not slowed by the other, and does not slow it.
BOOST_AUTO_TEST_CASE(EachApplicationHasItsOwnLimitForASite) {

    freshDatabase();
    const std::string site = "shared.example.com";
    setLimits(site, {.requestsPerSecond = 0, .burst = 1, .concurrent = 1}, kDownloader);
    setLimits(site, {.requestsPerSecond = 0, .burst = 1, .concurrent = 3}, kIndexer);

    BOOST_TEST(tryAcquire(site, "d1", kDownloader).has_value());
    BOOST_TEST(!tryAcquire(site, "d2", kDownloader).has_value());

    // The downloader's one slot is taken; the indexer's three are untouched by it.
    BOOST_TEST(tryAcquire(site, "i1", kIndexer).has_value());
    BOOST_TEST(tryAcquire(site, "i2", kIndexer).has_value());
    BOOST_TEST(tryAcquire(site, "i3", kIndexer).has_value());
    BOOST_TEST(!tryAcquire(site, "i4", kIndexer).has_value());
}

// An application's "*" limit is its default for every site without one of its own - and only its.
BOOST_AUTO_TEST_CASE(AnApplicationsDefaultAppliesToItsOtherSitesOnly) {

    freshDatabase();
    setLimits(kAllSites, {.requestsPerSecond = 0, .burst = 1, .concurrent = 1}, kDownloader);
    setLimits("fast.example.com", {.requestsPerSecond = 0, .burst = 1, .concurrent = 2}, kDownloader);

    // Any site: the application's default, one at a time.
    BOOST_TEST(tryAcquire("any.example.com", "a1").has_value());
    BOOST_TEST(!tryAcquire("any.example.com", "a2").has_value());

    // Its own limit wins over the default.
    BOOST_TEST(tryAcquire("fast.example.com", "f1").has_value());
    BOOST_TEST(tryAcquire("fast.example.com", "f2").has_value());

    // Another application is held to euclid's default - a burst of two - not to this one's one.
    BOOST_TEST(tryAcquire("any.example.com", "x1", kIndexer).has_value());
    BOOST_TEST(tryAcquire("any.example.com", "x2", kIndexer).has_value());
}
