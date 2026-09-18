// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ApplicationLoadTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>
#include <utility>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/entity/emm/Module.h>
#include <euclid/database/repository/emm/MongoEmmRepository.h>

using Euclid::Database::MongoEmmRepository;
using Euclid::Database::Entity::Module;
using Euclid::Database::Entity::ModuleInstance;
using Euclid::Database::Entity::ModuleState;

// An application receives no gateway request, so acquireInstance() never marks it busy and there is
// nothing for the autoscaler to read but what the application says about itself.
//
// It used to say it as an EMO metric. EMO accumulates samples in memory and writes a row only when
// its averaging bucket closes - every euclid.modules.emo.average-period seconds, five minutes as
// shipped - so a figure reported every fifteen seconds reached the manager up to five minutes late,
// and scaling was late by that much in both directions.
//
// It now writes to its own instance record, which the manager already reads on every reconcile.
// What that depends on, and what this pins: the write touches those fields and nothing else, and
// the manager's own writes do not erase them.

namespace {

    constexpr auto kModule = "billing-000000000000-production";
    constexpr auto kInstance = "instance-1";

    MongoEmmRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEmmRepository{};
    }

    Module moduleOf() {
        Module module;
        module.name = kModule;
        module.executable = "/usr/local/euclid/bin/java";
        module.minInstances = 1;
        module.maxInstances = 10;
        return module;
    }

    ModuleInstance instanceOf(const int pid = 4242) {
        ModuleInstance instance;
        instance.instanceId = kInstance;
        instance.pid = pid;
        instance.state = ModuleState::RUNNING;
        instance.socketPath = "/var/run/euclid/billing.4242.sock";
        instance.httpPort = 18080;
        return instance;
    }

    ModuleInstance reread(const MongoEmmRepository &repo) {
        for (const auto &module: repo.findAll()) {
            if (module.name != kModule) continue;
            for (const auto &instance: module.instances) {
                if (instance.instanceId == kInstance) return instance;
            }
        }
        BOOST_FAIL("instance not found");
        return {};
    }

}// namespace

BOOST_AUTO_TEST_CASE(AnInstanceThatHasNeverReportedSaysSo) {

    // -1 rather than 0: an application that has not spoken yet must not read as one reporting no
    // load at all, which is the difference between "nothing is known" and "it is idle" - and the
    // second would make it the first instance stopped.
    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);

    const auto stored = reread(repo);
    BOOST_TEST(stored.utilisation == -1.0);
    BOOST_TEST(stored.backlog == -1L);
    BOOST_TEST(stored.loadReportedAt.time_since_epoch().count() == 0);
}

BOOST_AUTO_TEST_CASE(AReportLandsOnTheInstanceRecord) {

    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);

    // Truncated to the millisecond BSON stores, so a timestamp taken at microsecond precision an
    // instant earlier can compare as later.
    const auto before = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    repo.reportInstanceLoad(kModule, kInstance, 72.5, 140, 0);

    const auto stored = reread(repo);
    BOOST_TEST(stored.utilisation == 72.5);
    BOOST_TEST(stored.backlog == 140L);

    // Stamped by the writer, not the reporter: an instance with a skewed clock would otherwise
    // report load that reads as stale for ever, or as fresh for ever.
    BOOST_TEST((stored.loadReportedAt >= before));
}

BOOST_AUTO_TEST_CASE(AReportTouchesNothingElseOnTheRecord) {

    // The reason this is a targeted update rather than an upsert of the instance. The manager owns
    // every other field and writes the whole subdocument; if the two wrote the same way, each
    // would erase the other's work - the pid and the socket path would flicker as the application
    // reported, which is what the autoscaler and the gateway route on.
    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);

    repo.reportInstanceLoad(kModule, kInstance, 40.0, 3, 0);

    const auto stored = reread(repo);
    BOOST_TEST(stored.pid == 4242);
    BOOST_TEST(stored.httpPort == 18080);
    BOOST_TEST(stored.socketPath == "/var/run/euclid/billing.4242.sock");
    BOOST_TEST((stored.state == ModuleState::RUNNING));
}

BOOST_AUTO_TEST_CASE(AReportCarriesWhatTheInstanceHasStartedAndNotFinished) {

    // The signal that keeps scale-down off an instance that is mid-flight. An application cannot
    // call reportBackgroundTasks() - that writes to the database, which an application has no
    // access to - so its load report is how it says the same thing, and for a deployed pool it is
    // the only writer of the field.
    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);

    repo.reportInstanceLoad(kModule, kInstance, 60.0, 12, 3);

    BOOST_TEST(reread(repo).backgroundTasks == 3L);

    // A gauge like the two beside it: what it is doing now, not what it has ever done.
    repo.reportInstanceLoad(kModule, kInstance, 0.0, 0, 0);

    BOOST_TEST(reread(repo).backgroundTasks == 0L);
}

BOOST_AUTO_TEST_CASE(TheManagersOwnWriteDoesNotEraseAReport) {

    // The other half of the same rule, and the one that would show up as scaling that works until
    // an instance changes state.
    //
    // Leaving the fields out of ModuleInstance::toDocument() is not what protects them - it is what
    // used to destroy them. The manager wrote `$set: {"instances.$": <the whole subdocument>}`,
    // which replaces the array element and drops every field the replacement does not carry. It now
    // sets the fields it owns one at a time.
    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);
    repo.reportInstanceLoad(kModule, kInstance, 88.0, 900, 0);

    // Something the manager does routinely: the instance's pid changes on a restart.
    auto restarted = instanceOf(5555);
    restarted.restartCount = 1;
    repo.upsertInstance(module, restarted);

    const auto stored = reread(repo);
    BOOST_TEST(stored.pid == 5555);
    BOOST_TEST(stored.utilisation == 88.0);
    BOOST_TEST(stored.backlog == 900L);
}

BOOST_AUTO_TEST_CASE(ReportingForAnInstanceThatIsNotThereDoesNothing) {

    // No upsert: a record the manager has not written yet is not a pool slot, and creating one
    // here would invent an instance the manager does not run.
    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);

    repo.reportInstanceLoad(kModule, "instance-does-not-exist", 50.0, 5, 0);

    const auto stored = reread(repo);
    BOOST_TEST(stored.utilisation == -1.0);

    for (const auto &found: repo.findAll()) {
        if (found.name != kModule) continue;
        BOOST_TEST(found.instances.size() == 1U);
    }
}

// ── What EMO records about a pool ───────────────────────────────────────────
//
// The mean for utilisation and the total for backlog. Getting these the wrong way round is the
// mistake euclid-spring's own comment warns about - it labels its metrics per instance precisely
// because EMO averages samples sharing a label, which would turn a backlog into a mean.

BOOST_AUTO_TEST_CASE(UtilisationIsTheMeanAndBacklogIsTheTotal) {

    Module module = moduleOf();
    for (const auto &[utilisation, backlog]: {std::pair{20.0, 100L}, std::pair{40.0, 200L}, std::pair{60.0, 300L}}) {
        auto instance = instanceOf();
        instance.instanceId = "inst-" + std::to_string(module.instances.size());
        instance.utilisation = utilisation;
        instance.backlog = backlog;
        module.instances.push_back(instance);
    }

    const auto load = Euclid::Database::Entity::SummarisePool(module);

    BOOST_TEST(load.running == 3L);
    BOOST_TEST(load.reporting == 3L);

    // Half a pool at 100% is a pool at 50%...
    BOOST_TEST(load.utilisation == 40.0);
    // ...but half a pool holding five hundred messages each is a thousand messages waiting.
    BOOST_TEST(load.backlog == 600L);
}

BOOST_AUTO_TEST_CASE(AnInstanceThatHasNeverReportedIsCountedButNotAveragedIn) {

    // Counting it as zero would let one silent instance halve the pool's reported load, and a
    // module that does not report at all would read as permanently idle.
    Module module = moduleOf();

    auto reporting = instanceOf();
    reporting.instanceId = "inst-reporting";
    reporting.utilisation = 80.0;
    reporting.backlog = 40;
    module.instances.push_back(reporting);

    auto silent = instanceOf();
    silent.instanceId = "inst-silent";
    module.instances.push_back(silent);

    const auto load = Euclid::Database::Entity::SummarisePool(module);

    BOOST_TEST(load.running == 2L);
    BOOST_TEST(load.reporting == 1L);
    BOOST_TEST(load.utilisation == 80.0);
    BOOST_TEST(load.backlog == 40L);
}

BOOST_AUTO_TEST_CASE(OnlyRunningInstancesCount) {

    Module module = moduleOf();

    auto running = instanceOf();
    running.instanceId = "inst-running";
    running.utilisation = 50.0;
    module.instances.push_back(running);

    auto stopped = instanceOf();
    stopped.instanceId = "inst-stopped";
    stopped.state = ModuleState::STOPPED;
    stopped.utilisation = 90.0;
    module.instances.push_back(stopped);

    const auto load = Euclid::Database::Entity::SummarisePool(module);

    BOOST_TEST(load.running == 1L);
    BOOST_TEST(load.utilisation == 50.0);
}

BOOST_AUTO_TEST_CASE(APoolThatReportsNothingStillHasACount) {

    // "module-instances" is recorded whatever happens - a pool that has scaled to nothing is what
    // somebody reading the graph is looking for, and a series that just stops saying anything
    // cannot be told apart from a collector that died.
    Module module = moduleOf();
    auto instance = instanceOf();
    instance.instanceId = "inst-silent";
    module.instances.push_back(instance);

    const auto load = Euclid::Database::Entity::SummarisePool(module);

    BOOST_TEST(load.running == 1L);
    BOOST_TEST(load.reporting == 0L);
    BOOST_TEST(load.utilisation == 0.0);
}

BOOST_AUTO_TEST_CASE(AnEmptyPoolSummarisesToNothing) {

    const auto load = Euclid::Database::Entity::SummarisePool(moduleOf());

    BOOST_TEST(load.running == 0L);
    BOOST_TEST(load.reporting == 0L);
    BOOST_TEST(load.backlog == 0L);
}

BOOST_AUTO_TEST_CASE(AReportSaysWhetherItLanded) {

    // A report written to a pool that does not exist used to be silent: the update matched nothing,
    // the caller was answered 200, and the autoscaler simply never saw the application. It happened
    // because EAP derived the pool from the caller's name - "app-parser" was taken to run a pool
    // called "parser", while the pool was "parser-dev" - and nine hundred reports in twenty minutes
    // went nowhere without one line anywhere saying so.
    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);

    BOOST_TEST(repo.reportInstanceLoad(kModule, kInstance, 50.0, 10, 1));
}

BOOST_AUTO_TEST_CASE(AReportForAPoolThatDoesNotExistSaysSo) {

    auto repo = freshRepository();
    repo.upsertInstance(moduleOf(), instanceOf());

    // The name nobody runs.
    BOOST_TEST(!repo.reportInstanceLoad("no-such-pool", kInstance, 50.0, 10, 1));
}

BOOST_AUTO_TEST_CASE(AReportForAnInstanceThePoolDoesNotHoldSaysSo) {

    // The benign version of the same miss - an instance reporting while its record is still being
    // written. Indistinguishable here from the misconfiguration above, which is why the caller
    // warns once per pool rather than once per report.
    auto repo = freshRepository();
    repo.upsertInstance(moduleOf(), instanceOf());

    BOOST_TEST(!repo.reportInstanceLoad(kModule, "instance-that-never-started", 50.0, 10, 1));
}

BOOST_AUTO_TEST_CASE(SuccessiveReportsReplaceRatherThanAccumulate) {

    // A gauge, not a counter: the newest figure is the whole answer, and the manager reads exactly
    // one number per instance per reconcile.
    auto repo = freshRepository();
    auto module = moduleOf();
    auto instance = instanceOf();
    repo.upsertInstance(module, instance);

    repo.reportInstanceLoad(kModule, kInstance, 90.0, 500, 0);
    repo.reportInstanceLoad(kModule, kInstance, 5.0, 0, 0);

    const auto stored = reread(repo);
    BOOST_TEST(stored.utilisation == 5.0);
    BOOST_TEST(stored.backlog == 0L);
}
