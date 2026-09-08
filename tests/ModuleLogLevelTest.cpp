#define BOOST_TEST_MODULE ModuleLogLevelTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/database/entity/emm/Module.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/emm/MongoEmmRepository.h>

using Euclid::Database::MongoEmmRepository;
using Euclid::Database::Entity::Module;
using Euclid::Database::Entity::ModuleInstance;
using Euclid::Database::Entity::ModuleState;

// A module's log level is set through EMM and read back by the manager on its next reconcile - so
// it has to survive the row it is stored in, and setting it must not disturb anything else the row
// carries. The manager sizes pools from the same document, and a level that quietly reset an
// instance limit would be a very expensive way to turn down a log.

namespace {

    Module demoModule() {
        Module module;
        module.name = "esm";
        module.executable = "/usr/local/euclid/bin/euclid-esm";
        module.socketPath = "/var/run/euclid/euclid-esm.sock";
        module.minInstances = 1;
        module.maxInstances = 10;
        module.desiredThreads = 8;
        module.logLevel = "warning";
        return module;
    }

    // The manager registers a module by reporting one of its instances, which is the only way a
    // row comes into existence - there is no "create module" anywhere.
    ModuleInstance demoInstance() {
        ModuleInstance instance;
        instance.instanceId = "i-1";
        instance.pid = 4711;
        instance.state = ModuleState::RUNNING;
        return instance;
    }

}// namespace

BOOST_AUTO_TEST_CASE(TheLevelSurvivesABsonRoundTrip) {

    const auto module = demoModule();
    const auto restored = Module::fromDocument(module.toDocument().view());

    BOOST_TEST(restored.logLevel == "warning");
    BOOST_TEST(restored.name == "esm");
    BOOST_TEST(restored.desiredThreads == 8);
}

BOOST_AUTO_TEST_CASE(AModuleWrittenBeforeTheFieldExistedHasNoLevel) {

    Module module = demoModule();
    module.logLevel.clear();

    // Which is what "leave it to euclid.logging.channels" looks like in a row, and what every
    // module in an installation that has never used set-log-level carries.
    const auto restored = Module::fromDocument(module.toDocument().view());
    BOOST_TEST(restored.logLevel.empty());
}

BOOST_AUTO_TEST_CASE(SettingTheLevelLeavesTheRestOfTheRowAlone) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEmmRepository repository;
    auto module = demoModule();
    module.logLevel.clear();
    repository.upsertInstance(module, demoInstance());
    BOOST_TEST_REQUIRE(repository.setDesiredThreads("esm", 8));

    BOOST_TEST_REQUIRE(repository.setLogLevel("esm", "off"));

    const auto stored = repository.findByName("esm");
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->logLevel == "off");

    // The manager sizes the pool and the worker threads from this same row on every tick.
    BOOST_TEST(stored->minInstances == 1);
    BOOST_TEST(stored->maxInstances == 10);
    BOOST_TEST(stored->desiredThreads == 8);
    BOOST_TEST(stored->instances.size() == 1U);
}

BOOST_AUTO_TEST_CASE(AnEmptyLevelTakesTheSettingBack) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEmmRepository repository;
    auto module = demoModule();
    repository.upsertInstance(module, demoInstance());
    BOOST_TEST_REQUIRE(repository.setLogLevel("esm", "off"));

    BOOST_TEST_REQUIRE(repository.setLogLevel("esm", ""));
    BOOST_TEST(repository.findByName("esm")->logLevel.empty());
}

BOOST_AUTO_TEST_CASE(AModuleNobodyKnowsIsReportedRatherThanCreated) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEmmRepository repository;

    BOOST_TEST(!repository.setLogLevel("nothing-of-that-name", "off"));
    BOOST_TEST(!repository.exists("nothing-of-that-name"));
}
