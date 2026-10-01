// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE InstanceHostTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/entity/emm/Module.h>
#include <euclid/database/repository/emm/MongoEmmRepository.h>

using Euclid::Database::MongoEmmRepository;
using Euclid::Database::Entity::Module;
using Euclid::Database::Entity::ModuleInstance;
using Euclid::Database::Entity::ModuleState;

// A Module document is keyed by module name, so every host running a module called "esm" shares one
// document and all their instances sit in the same array. Until an instance said which machine it
// was on, nothing in a record distinguished them - and two things act on that record destructively.
//
// killLeftoverInstances() takes a recorded pid, checks /proc/<pid>/exe against the module's
// executable, and kills the process if they match. Across hosts that check does not reject
// anything: the installation path is identical on every machine and pids collide freely. A manager
// starting up would SIGKILL a healthy process on the strength of a record a different machine
// wrote.
//
// The start-up clear() then deleted every module document in the collection, which on a shared
// database throws away the records of instances that are still running elsewhere - leaving
// processes nobody has a record of at all.
//
// These pin the field those two now turn on. The sweep itself is POSIX process handling and is not
// exercised here; what is exercised is the predicate it asks and the clear it performs.

namespace {

    constexpr auto kModule = "esm";
    constexpr auto kHere = "node-a";
    constexpr auto kElsewhere = "node-b";

    MongoEmmRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEmmRepository{};
    }

    Module moduleOf() {
        Module module;
        module.name = kModule;
        module.executable = "/usr/local/euclid/bin/euclid-esm";
        module.minInstances = 1;
        module.maxInstances = 10;
        return module;
    }

    ModuleInstance instanceOf(const std::string &instanceId, const std::string &host, const int pid) {
        ModuleInstance instance;
        instance.instanceId = instanceId;
        instance.pid = pid;
        instance.host = host;
        instance.state = ModuleState::RUNNING;
        return instance;
    }

    // Every instance of kModule still stored, whatever host it names.
    std::vector<ModuleInstance> stored(const MongoEmmRepository &repo) {
        for (const auto &module: repo.findAll()) {
            if (module.name == kModule) return module.instances;
        }
        return {};
    }

}// namespace

// ── The predicate ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ARecordNamingThisHostIsOurs) {

    BOOST_TEST(instanceOf("i-1", kHere, 100).isOn(kHere));
}

BOOST_AUTO_TEST_CASE(ARecordNamingAnotherHostIsNot) {

    // The whole point: this is the answer that stops a manager acting on a pid that means nothing
    // on the machine it is running on.
    BOOST_TEST(!instanceOf("i-1", kElsewhere, 100).isOn(kHere));
}

BOOST_AUTO_TEST_CASE(ARecordWithNoHostIsOurs) {

    // Every record written before the field existed has no host. Reading those as somebody else's
    // would leave a single-host installation unable to sweep its own leftovers after an upgrade -
    // they would be neither managed nor cleaned up, which is worse than the problem being fixed.
    BOOST_TEST(instanceOf("i-1", "", 100).isOn(kHere));
}

// ── What survives a round trip ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheHostIsStoredAndReadBack) {

    auto repo = freshRepository();
    const auto module = moduleOf();
    repo.upsertInstance(module, instanceOf("i-1", kHere, 100));

    const auto instances = stored(repo);
    BOOST_TEST_REQUIRE(instances.size() == 1U);
    BOOST_TEST(instances.front().host == kHere);
}

BOOST_AUTO_TEST_CASE(TwoHostsKeepSeparateInstancesOfOneModule) {

    // The arrangement the field exists for. One document, two machines, and each instance still
    // identifiable afterwards.
    auto repo = freshRepository();
    const auto module = moduleOf();
    repo.upsertInstance(module, instanceOf("i-1", kHere, 100));
    repo.upsertInstance(module, instanceOf("i-2", kElsewhere, 100));

    const auto instances = stored(repo);
    BOOST_TEST_REQUIRE(instances.size() == 2U);

    // Deliberately the same pid on both: that is the collision that made this necessary, and the
    // host is the only thing that tells them apart.
    for (const auto &instance: instances) {
        BOOST_TEST(instance.pid == 100);
        BOOST_TEST(instance.isOn(instance.instanceId == "i-1" ? kHere : kElsewhere));
    }
}

// ── The start-up clear ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ClearingThisHostLeavesTheOtherHostsInstances) {

    auto repo = freshRepository();
    const auto module = moduleOf();
    repo.upsertInstance(module, instanceOf("i-1", kHere, 100));
    repo.upsertInstance(module, instanceOf("i-2", kElsewhere, 200));

    repo.clearInstancesOn(kHere);

    const auto instances = stored(repo);
    BOOST_TEST_REQUIRE(instances.size() == 1U);
    BOOST_TEST(instances.front().instanceId == "i-2");
    BOOST_TEST(instances.front().host == kElsewhere);
}

BOOST_AUTO_TEST_CASE(ClearingThisHostTakesItsOwnUnhostedRecords) {

    // A record from before the field existed is this host's - see ARecordWithNoHostIsOurs. If the
    // clear skipped it, a single-host installation would accumulate the leftovers of every run it
    // ever made, and killLeftoverInstances would keep finding pids that belong to something else.
    auto repo = freshRepository();
    const auto module = moduleOf();
    repo.upsertInstance(module, instanceOf("i-legacy", "", 100));
    repo.upsertInstance(module, instanceOf("i-2", kElsewhere, 200));

    repo.clearInstancesOn(kHere);

    const auto instances = stored(repo);
    BOOST_TEST_REQUIRE(instances.size() == 1U);
    BOOST_TEST(instances.front().instanceId == "i-2");
}

BOOST_AUTO_TEST_CASE(ClearingLeavesTheModuleDocumentItself) {

    // The module row is shared by every host and its fields are not this host's to delete. The old
    // clear() dropped the whole document, which on a shared database took other hosts' live
    // instances with it.
    auto repo = freshRepository();
    const auto module = moduleOf();
    repo.upsertInstance(module, instanceOf("i-1", kHere, 100));

    repo.clearInstancesOn(kHere);

    bool found = false;
    for (const auto &stored: repo.findAll()) {
        if (stored.name != kModule) continue;
        found = true;
        BOOST_TEST(stored.instances.empty());
        BOOST_TEST(stored.executable == module.executable);
    }
    BOOST_TEST(found, "the module document was deleted along with its instances");
}

BOOST_AUTO_TEST_CASE(ClearingDoesNotEraseAnotherHostsLoadReport) {

    // The survivors are written back as the raw array elements that were read, never rebuilt from
    // ModuleInstance::toDocument() - which deliberately omits the four fields an instance reports
    // about itself, because the manager does not own them. Rebuilding would erase another host's
    // load figures, and the instance would write them again on its next tick: a number that
    // flickers to nothing rather than one that is plainly absent, which is far worse to diagnose.
    auto repo = freshRepository();
    const auto module = moduleOf();
    repo.upsertInstance(module, instanceOf("i-1", kHere, 100));
    repo.upsertInstance(module, instanceOf("i-2", kElsewhere, 200));

    BOOST_TEST_REQUIRE(repo.reportInstanceLoad(kModule, "i-2", 0.75, 42L, 3L));

    repo.clearInstancesOn(kHere);

    const auto instances = stored(repo);
    BOOST_TEST_REQUIRE(instances.size() == 1U);
    BOOST_TEST(instances.front().instanceId == "i-2");
    BOOST_TEST(instances.front().utilisation == 0.75);
    BOOST_TEST(instances.front().backlog == 42L);
}

BOOST_AUTO_TEST_CASE(ClearingAHostThatHasNothingChangesNothing) {

    auto repo = freshRepository();
    const auto module = moduleOf();
    repo.upsertInstance(module, instanceOf("i-2", kElsewhere, 200));

    repo.clearInstancesOn("node-c");

    const auto instances = stored(repo);
    BOOST_TEST_REQUIRE(instances.size() == 1U);
    BOOST_TEST(instances.front().instanceId == "i-2");
}
