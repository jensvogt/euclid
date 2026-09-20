// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ApplicationCleanupTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <set>
#include <string>

// Euclid includes
#include <euclid/manager/ApplicationCleanup.h>

using Euclid::main::DepartedApplicationPools;

// Deleting an application has to take its data directory with it, or the directories accumulate
// one per application ever deleted and a new application of the same name starts up on a dead
// one's files. This decides which ones go.
//
// The first version of this asked the wrong question - it looked for a *pool* the controller still
// ran that EAP no longer defined - and it was wrong in exactly the case that matters. Stopping an
// application already deregisters its pool, so a stopped application that is then deleted has no
// pool left to orphan, and stop-check-delete is how anybody removes one. It cleaned up only after
// a delete-while-running, which is the case nobody uses. That is the first test below.

BOOST_AUTO_TEST_CASE(AnApplicationDeletedWhileRunningIsCleanedUp) {

    const std::set<std::string> known{"orders", "billing"};
    const std::set<std::string> defined{"orders"};

    const auto departed = DepartedApplicationPools(known, defined);
    BOOST_REQUIRE(departed.size() == 1U);
    BOOST_TEST(departed.front() == "billing");
}

BOOST_AUTO_TEST_CASE(AnApplicationStoppedBeforeItWasDeletedIsCleanedUpToo) {

    // The regression. Stopping deregisters the pool, so nothing about the live pools says this
    // application ever existed - only the remembered name does. Asked of pools rather than names,
    // this returns nothing and the directory survives the delete.
    const std::set<std::string> known{"orders", "protocolizing"};
    const std::set<std::string> defined{"orders"};

    const auto departed = DepartedApplicationPools(known, defined);
    BOOST_REQUIRE(departed.size() == 1U);
    BOOST_TEST(departed.front() == "protocolizing");
}

BOOST_AUTO_TEST_CASE(NothingIsRemovedWhileEveryApplicationIsStillDefined) {

    const std::set<std::string> known{"orders", "billing", "protocolizing"};
    BOOST_TEST(DepartedApplicationPools(known, known).empty());
}

BOOST_AUTO_TEST_CASE(TheLastApplicationStillCleansUpWhenItGoes) {

    // The one case where "nothing is defined" is the honest answer rather than a failed read, and
    // the guard below must not swallow it.
    const std::set<std::string> known{"orders"};
    const auto departed = DepartedApplicationPools(known, {});
    BOOST_REQUIRE(departed.size() == 1U);
    BOOST_TEST(departed.front() == "orders");
}

BOOST_AUTO_TEST_CASE(ADatabaseThatAnswersNothingDoesNotDeleteEverything) {

    // What this guards is a recursive delete of every application's data on this host. The list of
    // definitions comes from a single unguarded repository read; if that ever answers "no
    // applications" because it failed rather than because there are none, every remembered name
    // looks deleted in the same pass.
    //
    // An installation losing all of its applications between two reconciles is not a thing that
    // happens. A database read failing is. So the pass does nothing and waits for the next one -
    // a directory that lives an extra minute costs nothing, and the alternative is unrecoverable.
    const std::set<std::string> known{"orders", "billing", "protocolizing", "splitting"};
    BOOST_TEST(DepartedApplicationPools(known, {}).empty());
}

BOOST_AUTO_TEST_CASE(NothingRememberedMeansNothingToRemove) {

    // A manager that has just started remembers no pools, so a reconcile pass finding applications
    // it never registered must not conclude anything about directories on disk.
    BOOST_TEST(DepartedApplicationPools({}, {"orders"}).empty());
    BOOST_TEST(DepartedApplicationPools({}, {}).empty());
}

BOOST_AUTO_TEST_CASE(EveryDepartedNameIsReportedRatherThanTheFirst) {

    // Two applications deleted between one pass and the next is ordinary - a redeploy script that
    // removes several - and returning only one would strand the rest until something else noticed.
    const std::set<std::string> known{"a", "b", "c", "d"};
    const std::set<std::string> defined{"b"};

    const auto departed = DepartedApplicationPools(known, defined);
    BOOST_REQUIRE(departed.size() == 3U);
    BOOST_TEST(departed[0] == "a");
    BOOST_TEST(departed[1] == "c");
    BOOST_TEST(departed[2] == "d");
}
