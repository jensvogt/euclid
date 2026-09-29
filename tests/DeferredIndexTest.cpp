// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE DeferredIndexTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <tuple>
#include <vector>

// Euclid includes
#include <euclid/database/Database.h>

using Euclid::Database::Database;

// Every repository creates its indexes in its constructor, and on the EMD backend the manager
// constructs its repositories before it starts the module that holds the store. So the creation was
// attempted against a socket that did not exist yet, logged one error, and - being a constructor -
// was never attempted again:
//
//   Ensure module indexes failed, error: Could not reach the memory database at
//   /var/run/euclid/euclid-emd.sock: No such file or directory
//
// The unique index on emm_module.name then did not exist for the life of that process, with nothing
// after that one line to say so. What is pinned here is the ordering that fixes it.
//
// Database is a singleton, so this process has exactly one "not reachable yet" to spend and the
// first case spends it. The cases after it run against a backend that is already up, which is the
// other half of the contract and is said plainly in each rather than dressed up as the first.

BOOST_AUTO_TEST_SUITE(DeferredIndexTest)

    BOOST_AUTO_TEST_CASE(WorkWaitsForABackendAndMayThenUseIt) {

        // Registered before any initialize*(), which is the manager's own order.
        std::vector<std::string> order;
        Database::instance().onReachable([&order] {
            // The reason the queue is drained outside the mutex: index creation is a store call, and
            // on the EMD backend that call's own reply re-enters the flush. Holding the lock across
            // this would deadlock the first module to come up.
            std::ignore = Database::instance().collection("deferred_index_test");
            order.emplace_back("used the store");
            Database::instance().onReachable([&order] { order.emplace_back("and again"); });
        });

        BOOST_TEST(order.empty(), "there is nothing to run it against yet, so it must not have run");

        // The in-process store is up the moment it exists, so this is where the wait ends. On the
        // EMD backend the same flush happens on the store's first reply instead.
        Database::instance().initializeMemory();

        BOOST_REQUIRE(order.size() == 2U);
        BOOST_TEST(order[0] == "used the store");

        // Registered from inside the flush, by which time the backend is reachable - so it ran
        // there and then rather than joining a queue that has already been drained and will not be
        // drained again.
        BOOST_TEST(order[1] == "and again");
    }

    BOOST_AUTO_TEST_CASE(WorkRegisteredWhenTheBackendIsUpRunsAtOnce) {

        // What every backend but EMD does, and what a repository constructor has always done: the
        // indexes are created then and there. The backend is reachable by now - see the note above.
        int ran = 0;
        Database::instance().onReachable([&ran] { ++ran; });
        BOOST_TEST(ran == 1);
    }

    BOOST_AUTO_TEST_CASE(FurtherAnswersDoNotRunItAgain) {

        // The hook fires on every reply, not only the first, because deciding "first" is this side's
        // job - an index creation per store call would add a round trip to every query. So work runs
        // once however many times the backend says it is there.
        int ran = 0;
        Database::instance().onReachable([&ran] { ++ran; });
        Database::instance().initializeMemory();
        Database::instance().initializeMemory();

        BOOST_TEST(ran == 1);
    }

BOOST_AUTO_TEST_SUITE_END()
