// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE SchedulerShutdownTest

// Boost includes
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
#include <chrono>
#include <thread>

// Euclid includes
#include <euclid/core/Scheduler.h>

using namespace std::chrono_literals;
using Euclid::Core::Scheduler;

namespace {

    // Static storage duration on purpose. A task thread is detached, so if the barrier under test
    // ever regresses these are touched after the test that created them has returned - and a flag
    // that has itself been destroyed turns a failure into a crash with no message. std::atomic<bool>
    // is trivially destructible, so it stays readable to the end of the process either way.
    std::atomic<bool> g_started{false};
    std::atomic<bool> g_finished{false};

    bool waitFor(const std::atomic<bool> &flag, const std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (flag.load()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return flag.load();
    }

}// namespace

BOOST_AUTO_TEST_SUITE(SchedulerShutdownTest)

BOOST_AUTO_TEST_CASE(StopWaitsForATaskThatHasAlreadyStarted) {

    // The regression this exists for. Task threads are detached, and Stop() used to join only the
    // timer thread - so a task could still be reading a singleton while the process tore that
    // singleton down around it. It reached production as "metrics-push-eap ... string too large":
    // MonitoringCollector's map was gone, the strings read back as garbage, and boost::json
    // refused one two billion bytes long. Stop() has to be a barrier, not a flag.
    g_started = false;
    g_finished = false;

    auto &scheduler = Scheduler::instance();
    scheduler.Start();
    scheduler.ScheduleOnce(
            "slow-task",
            [] {
                g_started = true;
                std::this_thread::sleep_for(300ms);
                g_finished = true;
            },
            0ms);

    BOOST_REQUIRE(waitFor(g_started, 2000ms));

    scheduler.Stop();

    // Not "eventually" - by the time Stop() returns. Anything else and shutdown continues while
    // the task is still running, which is the whole bug.
    BOOST_TEST(g_finished.load());
}

BOOST_AUTO_TEST_CASE(StopGivesUpRatherThanHoldingTheProcessOpen) {

    // The other half. The manager SIGKILLs an instance that has not stopped in five seconds, so a
    // barrier that waits for a task blocked on something that will never answer would turn every
    // shutdown into a kill - trading one bad outcome for another. It waits, then says what it is
    // still waiting for and lets the process go.
    g_started = false;
    g_finished = false;

    auto &scheduler = Scheduler::instance();
    scheduler.Start();
    scheduler.ScheduleOnce(
            "wedged-task",
            [] {
                g_started = true;
                std::this_thread::sleep_for(3500ms);
                g_finished = true;
            },
            0ms);

    BOOST_REQUIRE(waitFor(g_started, 2000ms));

    const auto before = std::chrono::steady_clock::now();
    scheduler.Stop();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before);

    // The grace period is 2000ms; generous slack either side because this measures a sleeping
    // thread on a loaded build machine. What matters is that it is bounded and that it did wait.
    BOOST_TEST(waited.count() >= 1500);
    BOOST_TEST(waited.count() < 3000);
    BOOST_TEST(!g_finished.load());

    // Let the wedged task finish before the process exits, so it is not still running during
    // static destruction - which is precisely the thing this file is about.
    BOOST_TEST(waitFor(g_finished, 4000ms));
}

BOOST_AUTO_TEST_CASE(StoppingTwiceIsHarmless) {

    // Stop() is reached from two directions now - UnixSocketServer::stop() on the way down, and
    // ~Scheduler() during static destruction after it. The second one has to be a no-op.
    auto &scheduler = Scheduler::instance();
    scheduler.Start();
    scheduler.Stop();
    scheduler.Stop();
    BOOST_TEST(!scheduler.IsRunning());
}

BOOST_AUTO_TEST_SUITE_END()
