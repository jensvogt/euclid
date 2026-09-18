// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE StickyCallTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <TransferContext.h>

using Euclid::Transfer::ModuleResponse;
using Euclid::Transfer::Detail::StickyCall;

// A multipart transfer is a long sequence of calls against one module. It used to hold one
// instance's socket for all of them, which made a 12 GB upload only as durable as the instance it
// happened to start on: fifteen hundred parts over about ninety seconds, during which the upload's
// own request load ramps ESM from one instance to ten. Stop that particular one and every part
// already sent is lost - there is no resume. Seen as "upload-part ... Broken pipe" at part 566 of
// an 11.5 GiB file.
//
// What this pins is the four things that make carrying on possible, and cheap.

namespace {

    ModuleResponse ok(const int status = 200) { return {.status = status, .body = {}}; }

    // What CallModuleAt() returns when the instance cannot be reached at all.
    ModuleResponse unreachable() { return {}; }

    // Records what was tried, so a test can assert on the order and the count rather than only on
    // the answer.
    struct Calls {
        std::vector<std::string> tried;
        int resolves = 0;
    };

}// namespace

BOOST_AUTO_TEST_SUITE(StickyCallTest)

// ── The common case must not cost a lookup ──────────────────────────────────

BOOST_AUTO_TEST_CASE(TheRememberedInstanceIsUsedWithoutResolving) {

    // Resolving reads every module's record from the database. A 12 GB upload is fifteen hundred
    // parts, and doing it per part would be fifteen hundred queries for one file - which is why
    // this is sticky rather than simply asking each time.
    Calls calls;
    std::string socket = "esm.1.sock";

    const auto response = StickyCall(
            socket,
            [&](const std::string &s) { calls.tried.push_back(s); return ok(); },
            [&] { ++calls.resolves; return std::vector<std::string>{"esm.1.sock", "esm.2.sock"}; });

    BOOST_TEST(response.status == 200);
    BOOST_TEST(calls.resolves == 0);
    BOOST_TEST(calls.tried.size() == 1U);
    BOOST_TEST(socket == "esm.1.sock");
}

// ── When the instance goes away ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnInstanceThatStoppedAnsweringIsReplacedRatherThanFatal) {

    // The whole point: the autoscaler stopping the instance an upload happens to be on must cost
    // that one part's retry, not the whole transfer.
    Calls calls;
    std::string socket = "esm.1.sock";

    const auto response = StickyCall(
            socket,
            [&](const std::string &s) {
                calls.tried.push_back(s);
                return s == "esm.1.sock" ? unreachable() : ok();
            },
            [&] { ++calls.resolves; return std::vector<std::string>{"esm.1.sock", "esm.2.sock"}; });

    BOOST_TEST(response.status == 200);
    BOOST_TEST(calls.resolves == 1);
    BOOST_TEST(socket == "esm.2.sock");
}

BOOST_AUTO_TEST_CASE(TheInstanceThatJustFailedIsNotTriedTwice) {

    // Reaching a dead socket is not instant - it is a connect that has to fail. Trying it again
    // inside the same call pays for that twice, per part, for the rest of the transfer.
    Calls calls;
    std::string socket = "esm.1.sock";

    std::ignore = StickyCall(
            socket,
            [&](const std::string &s) {
                calls.tried.push_back(s);
                return s == "esm.1.sock" ? unreachable() : ok();
            },
            [&] { return std::vector<std::string>{"esm.1.sock", "esm.2.sock"}; });

    const auto firstTried = std::ranges::count(calls.tried, std::string{"esm.1.sock"});
    BOOST_TEST(firstTried == 1L);
}

BOOST_AUTO_TEST_CASE(WhicheverAnsweredIsTheOneRememberedNext) {

    // Otherwise every subsequent part pays the dead instance's failure again before moving on.
    Calls calls;
    std::string socket = "esm.1.sock";

    std::ignore = StickyCall(
            socket,
            [&](const std::string &s) { return s == "esm.3.sock" ? ok() : unreachable(); },
            [&] { return std::vector<std::string>{"esm.1.sock", "esm.2.sock", "esm.3.sock"}; });
    BOOST_TEST(socket == "esm.3.sock");

    // And the next call goes straight there.
    const auto response = StickyCall(
            socket,
            [&](const std::string &s) { calls.tried.push_back(s); return ok(); },
            [&] { ++calls.resolves; return std::vector<std::string>{}; });

    BOOST_TEST(response.status == 200);
    BOOST_TEST(calls.resolves == 0);
}

// ── Nothing left to carry on with ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(NoInstanceAnsweringIsReportedAsAFailedCall) {

    // Status zero is what the caller checks; the transfer then fails honestly rather than
    // appearing to have written a part nobody took.
    std::string socket = "esm.1.sock";

    const auto response = StickyCall(
            socket,
            [](const std::string &) { return unreachable(); },
            [] { return std::vector<std::string>{"esm.1.sock", "esm.2.sock"}; });

    BOOST_TEST(response.status == 0);
}

BOOST_AUTO_TEST_CASE(AFirstCallWithNothingRememberedResolves) {

    // The start of a transfer, and the path taken after a restart of the process holding it.
    Calls calls;
    std::string socket;

    const auto response = StickyCall(
            socket,
            [&](const std::string &s) { calls.tried.push_back(s); return ok(); },
            [&] { ++calls.resolves; return std::vector<std::string>{"esm.7.sock"}; });

    BOOST_TEST(response.status == 200);
    BOOST_TEST(calls.resolves == 1);
    BOOST_TEST(socket == "esm.7.sock");
}

BOOST_AUTO_TEST_SUITE_END()
