// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BackgroundWorkTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/BackgroundWork.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/emm/Module.h>
#include <euclid/database/repository/emm/MongoEmmRepository.h>

using Euclid::Database::BackgroundWork;
using Euclid::Database::MongoEmmRepository;
using Euclid::Database::Entity::Module;
using Euclid::Database::Entity::ModuleInstance;
using Euclid::Database::Entity::ModuleState;

// An --async command is answered at once and carried out on a detached thread. From outside the
// process that thread does not exist, so the autoscaler stops an instance it sees no requests on
// and the work stops with it - mid-purge, mid-resend. backgroundTasks is how a module says it is
// still doing something, and evaluateScaling leaves such an instance alone.
//
// What this pins is that the count comes back down on every path, which is the half that was got
// wrong: ESM had four places that raised it and three that lowered it, and one command raised a
// counter that was never reported at all, so that command never protected its instance.

namespace {

    // BackgroundWork reports under the instance name the manager gave the process. The tests do
    // not set EUCLID_INSTANCE_ID, so it falls back to the pid form - which is exactly what a
    // module started by hand gets, and is why the fixture asks the repository for it rather than
    // assuming a literal.
    constexpr auto kModule = "esm";

    MongoEmmRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();

        MongoEmmRepository repo;

        Module module;
        module.name = kModule;
        module.executable = "/usr/local/euclid/bin/euclid-esm";
        module.minInstances = 1;
        module.maxInstances = 10;

        ModuleInstance instance;
        instance.instanceId = Euclid::Database::InstanceName();
        instance.pid = 4242;
        instance.state = ModuleState::RUNNING;

        repo.upsertInstance(module, instance);
        return repo;
    }

    long reportedTasks(const MongoEmmRepository &repo) {
        const auto module = repo.findByName(kModule);
        if (!module.has_value()) return -1;
        for (const auto &instance: module->instances) {
            if (instance.instanceId == Euclid::Database::InstanceName()) return instance.backgroundTasks;
        }
        return -1;
    }

}// namespace

BOOST_AUTO_TEST_SUITE(BackgroundWorkTest)

// ── The count goes up and comes back down ───────────────────────────────────

BOOST_AUTO_TEST_CASE(TakingWorkReportsItAtOnce) {

    // At once, not on the next tick: the request that started the work returns immediately, and a
    // scale-down decision one second later has to read something current.
    auto repo = freshRepository();
    BOOST_TEST(reportedTasks(repo) == 0L);

    const auto work = BackgroundWork::Begin(kModule);

    BOOST_TEST(reportedTasks(repo) == 1L);
}

BOOST_AUTO_TEST_CASE(ReleasingWorkReportsItAgain) {

    auto repo = freshRepository();
    {
        const auto work = BackgroundWork::Begin(kModule);
        BOOST_TEST(reportedTasks(repo) == 1L);
    }

    BOOST_TEST(reportedTasks(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(SeveralTasksAtOnceAreCountedTogether) {

    // One counter for the whole process rather than one per command: the manager's question is
    // "is this instance busy with something I cannot see", and two purges and a resend answer it
    // the same way.
    auto repo = freshRepository();

    const auto purge = BackgroundWork::Begin(kModule);
    const auto touch = BackgroundWork::Begin(kModule);
    const auto resend = BackgroundWork::Begin(kModule);

    BOOST_TEST(reportedTasks(repo) == 3L);
}

BOOST_AUTO_TEST_CASE(TasksFinishingOutOfOrderStillReachZero) {

    auto repo = freshRepository();

    auto first = BackgroundWork::Begin(kModule);
    auto second = BackgroundWork::Begin(kModule);
    BOOST_TEST(reportedTasks(repo) == 2L);

    // The one that started first finishes last, which is the normal case: a purge of a large
    // bucket outlives a resend started after it.
    second.reset();
    BOOST_TEST(reportedTasks(repo) == 1L);

    first.reset();
    BOOST_TEST(reportedTasks(repo) == 0L);
}

// ── The paths that were got wrong ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnExceptionStillReleasesTheWork) {

    // The reason this is a destructor rather than a pair of calls. A worker that throws must not
    // leave the instance looking permanently busy - that would pin a pool at its current size for
    // as long as the process lives.
    auto repo = freshRepository();

    try {
        const auto work = BackgroundWork::Begin(kModule);
        BOOST_TEST(reportedTasks(repo) == 1L);
        throw std::runtime_error("worker failed");
    } catch (const std::runtime_error &) {
        // Swallowed exactly as a detached thread's entry function has to swallow it.
    }

    BOOST_TEST(reportedTasks(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(AnEarlyReturnStillReleasesTheWork) {

    // workPurgeJob() returns early in two places - a bucket that is gone, and a claim lost to
    // another instance - and each one used to need its own decrement. Two of three is how the
    // count drifts.
    auto repo = freshRepository();

    const auto worker = [&repo](const bool giveUpEarly) {
        const auto work = BackgroundWork::Begin(kModule);
        BOOST_TEST(reportedTasks(repo) == 1L);
        if (giveUpEarly) return;
    };

    worker(true);
    BOOST_TEST(reportedTasks(repo) == 0L);

    worker(false);
    BOOST_TEST(reportedTasks(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(TheHandleHeldByAThreadKeepsTheWorkCounted) {

    // How it is actually used: the handle is taken before the thread starts, so the count is
    // already up when the request returns, and captured by the thread's lambda so it comes down
    // when the lambda is destroyed rather than when the starting function returns.
    auto repo = freshRepository();

    std::vector<std::shared_ptr<BackgroundWork>> thread;
    {
        const auto work = BackgroundWork::Begin(kModule);
        thread.push_back(work);// what capturing it by value into the lambda does
    }

    // The starting scope is gone; the "thread" still holds it.
    BOOST_TEST(reportedTasks(repo) == 1L);

    thread.clear();
    BOOST_TEST(reportedTasks(repo) == 0L);
}

// ── A slot outlives the process that reported into it ───────────────────────

BOOST_AUTO_TEST_CASE(ARestartedSlotDoesNotInheritTheDeadProcessesCount) {

    // The bug this pins, seen in production: an ENS instance was restarted while two --async
    // resends were running. The threads died with the process, but instanceId is deliberately
    // stable across restarts, so the new process came up owning a record that still said
    // backgroundTasks=2 - and evaluateScaling() never scales such an instance down. The count
    // lives in the process; a process that is killed never gets to count down, so nothing in the
    // module can correct this and the pin would have lasted the life of the installation.
    auto repo = freshRepository();

    // The dead process left two behind.
    const auto first = BackgroundWork::Begin(kModule);
    const auto second = BackgroundWork::Begin(kModule);
    BOOST_TEST(reportedTasks(repo) == 2L);

    // The manager spawns into the same slot, which is the one moment it knows the figures belong
    // to nobody.
    repo.clearInstanceReports(kModule, Euclid::Database::InstanceName());

    BOOST_TEST(reportedTasks(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(ClearingAlsoForgetsWhatTheDeadProcessSaidAboutItsLoad) {

    // Same reasoning, one step further: utilisation and backlog describe a process that is gone
    // too. They are put back to "never reported" rather than to zero - a utilisation of 0 is a
    // figure the autoscaler acts on, and acting on a dead process's silence as though it were an
    // idle one is how a pool scales down under load.
    auto repo = freshRepository();

    repo.reportInstanceLoad(kModule, Euclid::Database::InstanceName(), 90.0, 500, 3);
    repo.clearInstanceReports(kModule, Euclid::Database::InstanceName());

    const auto module = repo.findByName(kModule);
    BOOST_REQUIRE(module.has_value());
    BOOST_REQUIRE(!module->instances.empty());

    const auto &instance = module->instances.front();
    BOOST_TEST(instance.backgroundTasks == 0L);
    BOOST_TEST(instance.utilisation < 0);
    BOOST_TEST(instance.backlog < 0);
}

BOOST_AUTO_TEST_SUITE_END()
