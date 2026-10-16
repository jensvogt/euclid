// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EgressPermitsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <EgressPermits.h>

using namespace Euclid::EAP::Egress;
using namespace std::chrono_literals;

// The permits euclid's applications ask for before calling an outside site. What a site sees is
// the sum over every instance asking, so what is pinned here is the arithmetic of that sum: how
// many go through, when the rest may ask again, and that nothing lets through more than the
// limits say - however the requests are spread over time.

namespace {

    const Clock::time_point t0 = Clock::time_point{} + 1'000'000h;// any time that is not the epoch

    Decision ask(SiteState &state, const Limits &limits, const Clock::time_point now, const std::string &id = "p",
                 const std::chrono::seconds lease = 900s) {
        return Acquire(state, limits, now, id, lease);
    }

}// namespace

BOOST_AUTO_TEST_CASE(AFirstRequestFindsTheBucketFull) {

    SiteState state;
    const Limits limits{.requestsPerSecond = 1, .burst = 3, .concurrent = 0};

    for (int i = 0; i < 3; ++i) BOOST_TEST(ask(state, limits, t0).granted);
    const auto fourth = ask(state, limits, t0);
    BOOST_TEST(!fourth.granted);
    BOOST_TEST(RefusalToString(fourth.refusal) == "rate");
    BOOST_TEST(fourth.retryAfter.count() == 1000);
}

BOOST_AUTO_TEST_CASE(TheBucketRefillsAtTheRate) {

    SiteState state;
    const Limits limits{.requestsPerSecond = 2, .burst = 1, .concurrent = 0};

    BOOST_TEST(ask(state, limits, t0).granted);
    const auto early = ask(state, limits, t0 + 200ms);
    BOOST_TEST(!early.granted);
    BOOST_TEST(early.retryAfter.count() == 300);
    BOOST_TEST(ask(state, limits, t0 + 500ms).granted);
}

// The property the whole thing exists for: over any stretch of time, no more go through than the
// burst plus the rate times its length - asked as often as anybody likes.
BOOST_AUTO_TEST_CASE(NoMoreThanTheRateGoesThroughHoweverOftenItIsAsked) {

    SiteState state;
    const Limits limits{.requestsPerSecond = 0.5, .burst = 2, .concurrent = 0};

    int granted = 0;
    for (auto t = t0; t < t0 + 60s; t += 37ms) {
        if (ask(state, limits, t).granted) ++granted;
    }
    BOOST_TEST(granted <= 2 + 30);
    BOOST_TEST(granted >= 30);
}

BOOST_AUTO_TEST_CASE(ASlotIsHeldUntilReleased) {

    SiteState state;
    const Limits limits{.requestsPerSecond = 0, .burst = 1, .concurrent = 2};

    const auto a = ask(state, limits, t0, "a");
    const auto b = ask(state, limits, t0, "b");
    BOOST_TEST(a.granted);
    BOOST_TEST(b.granted);

    const auto c = ask(state, limits, t0, "c");
    BOOST_TEST(!c.granted);
    BOOST_TEST(RefusalToString(c.refusal) == "concurrency");
    BOOST_TEST(c.retryAfter <= kConcurrencyRetry);

    BOOST_TEST(Release(state, "a"));
    BOOST_TEST(ask(state, limits, t0, "c").granted);
}

// An instance killed in the middle of a download never releases. Its slot comes back when the
// lease runs out, or the site would be one slot down for good.
BOOST_AUTO_TEST_CASE(AnAbandonedSlotComesBackWhenItsLeaseRunsOut) {

    SiteState state;
    const Limits limits{.requestsPerSecond = 0, .burst = 1, .concurrent = 1};

    BOOST_TEST(ask(state, limits, t0, "killed", 10s).granted);
    BOOST_TEST(!ask(state, limits, t0 + 9s).granted);
    BOOST_TEST(ask(state, limits, t0 + 10s, "next").granted);
}

// Waiting for a slot must not use up the rate: otherwise a caller kept out by concurrency would
// find the bucket empty too the moment a slot came free.
BOOST_AUTO_TEST_CASE(ARefusedRequestTakesNoToken) {

    SiteState state;
    const Limits limits{.requestsPerSecond = 0.001, .burst = 2, .concurrent = 1};

    BOOST_TEST(ask(state, limits, t0, "a").granted);
    for (int i = 0; i < 5; ++i) BOOST_TEST(!ask(state, limits, t0).granted);
    BOOST_TEST(Release(state, "a"));
    BOOST_TEST(ask(state, limits, t0, "b").granted);
}

BOOST_AUTO_TEST_CASE(ReleasingAnUnknownPermitIsNotAnError) {

    SiteState state;
    BOOST_TEST(!Release(state, "never-granted"));
}

BOOST_AUTO_TEST_CASE(ABlockedSiteGrantsNothingUntilTheBlockEnds) {

    SiteState state;
    const Limits limits{.requestsPerSecond = 100, .burst = 100, .concurrent = 100};

    Block(state, t0, 30s);
    const auto refused = ask(state, limits, t0 + 10s);
    BOOST_TEST(!refused.granted);
    BOOST_TEST(RefusalToString(refused.refusal) == "blocked");
    BOOST_TEST(refused.retryAfter.count() == 20000);
    BOOST_TEST(ask(state, limits, t0 + 30s).granted);
}

// Two instances told different things by the same site: the longer wait wins.
BOOST_AUTO_TEST_CASE(AShorterReportDoesNotShortenABlock) {

    SiteState state;
    Block(state, t0, 120s);
    BOOST_TEST((Block(state, t0, 5s) == t0 + 120s));
}

BOOST_AUTO_TEST_CASE(ABlockIsCappedAtAnHour) {

    SiteState state;
    BOOST_TEST((Block(state, t0, std::chrono::hours{24 * 30}) == t0 + kMaxBlock));
}

BOOST_AUTO_TEST_CASE(ALoweredBurstShrinksAFullBucket) {

    SiteState state;
    ask(state, {.requestsPerSecond = 1, .burst = 10, .concurrent = 0}, t0);
    const Limits lowered{.requestsPerSecond = 1, .burst = 2, .concurrent = 0};
    BOOST_TEST(ask(state, lowered, t0).granted);
    BOOST_TEST(ask(state, lowered, t0).granted);
    BOOST_TEST(!ask(state, lowered, t0).granted);
}

BOOST_AUTO_TEST_CASE(NoLimitsMeansEverythingGoesThrough) {

    SiteState state;
    const Limits unlimited{.requestsPerSecond = 0, .burst = 1, .concurrent = 0};
    for (int i = 0; i < 1000; ++i) BOOST_TEST_REQUIRE(ask(state, unlimited, t0, std::to_string(i)).granted);
}

BOOST_AUTO_TEST_CASE(ASiteIsItsHostLowerCased) {

    BOOST_TEST(NormalizeSite("Downloads.Example.COM.") == "downloads.example.com");
    BOOST_TEST(NormalizeSite("10.0.0.1") == "10.0.0.1");
    BOOST_TEST(NormalizeSite("").empty());
    BOOST_TEST(NormalizeSite("https://example.com/x").empty());
    BOOST_TEST(NormalizeSite("exa mple.com").empty());
    BOOST_TEST(NormalizeSite(std::string(254, 'a')).empty());
}
