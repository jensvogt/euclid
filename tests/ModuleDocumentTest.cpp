// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ModuleDocumentTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

// Euclid includes
#include <euclid/database/entity/emm/Module.h>

using Euclid::Database::Entity::Module;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

// findAll() is what every reconcile the manager makes is built on - what to start, what to stop,
// what to scale. It used to be all or nothing: the reader asked an element for a string without
// asking what it was first, so one field of one document with an unexpected type threw out of the
// whole loop, the repository caught it and answered with an empty vector, and the manager could not
// tell that apart from an installation with nothing running.
//
// A document euclid cannot fully make sense of should cost that field, or at worst that document,
// and never the listing.

BOOST_AUTO_TEST_CASE(AWellFormedDocumentReadsAsItAlwaysDid) {

    const auto doc = make_document(
            kvp("name", "billing"),
            kvp("executable", "/usr/local/euclid/bin/java"),
            kvp("socketPath", "/var/run/euclid/billing.sock"),
            kvp("active", true),
            kvp("minInstances", 1),
            kvp("maxInstances", 5),
            kvp("args", [](bsoncxx::builder::basic::sub_array sa) { sa.append("--server"); sa.append("--port"); }));

    const auto module = Module::fromDocument(doc.view());

    BOOST_TEST(module.name == "billing");
    BOOST_TEST(module.executable == "/usr/local/euclid/bin/java");
    BOOST_TEST(module.active);
    BOOST_TEST(module.minInstances == 1);
    BOOST_TEST(module.args.size() == 2U);
}

BOOST_AUTO_TEST_CASE(AFieldOfTheWrongTypeCostsThatFieldAndNothingElse) {

    // A null where a name should be, a number where a path should be. Whatever put them there, the
    // rest of the document still says what the manager needs to run the module.
    const auto doc = make_document(
            kvp("name", "billing"),
            kvp("executable", bsoncxx::types::b_null{}),
            kvp("socketPath", 42),
            kvp("active", true),
            kvp("maxInstances", 5));

    Module module;
    BOOST_REQUIRE_NO_THROW(module = Module::fromDocument(doc.view()));

    BOOST_TEST(module.name == "billing");
    BOOST_TEST(module.executable.empty());
    BOOST_TEST(module.socketPath.empty());
    BOOST_TEST(module.active);
    BOOST_TEST(module.maxInstances == 5);
}

BOOST_AUTO_TEST_CASE(AnArrayEntryOfTheWrongTypeIsSkipped) {

    const auto doc = make_document(
            kvp("name", "billing"),
            kvp("args", [](bsoncxx::builder::basic::sub_array sa) { sa.append("--server"); sa.append(7); sa.append("--port"); }));

    Module module;
    BOOST_REQUIRE_NO_THROW(module = Module::fromDocument(doc.view()));

    BOOST_TEST(module.args.size() == 2U);
    BOOST_TEST(module.args[0] == "--server");
    BOOST_TEST(module.args[1] == "--port");
}

BOOST_AUTO_TEST_CASE(AnInstancesEntryThatIsNotADocumentIsSkipped) {

    // Loses that one instance rather than every module in the installation.
    const auto doc = make_document(
            kvp("name", "billing"),
            kvp("instances", [](bsoncxx::builder::basic::sub_array sa) {
                sa.append(make_document(kvp("instanceId", "inst-1"), kvp("pid", 100), kvp("state", "RUNNING")));
                sa.append("not an instance");
            }));

    Module module;
    BOOST_REQUIRE_NO_THROW(module = Module::fromDocument(doc.view()));

    BOOST_REQUIRE(module.instances.size() == 1U);
    BOOST_TEST(module.instances.front().instanceId == "inst-1");
    BOOST_TEST(module.instances.front().pid == 100);
}

BOOST_AUTO_TEST_CASE(AnInstanceFieldOfTheWrongTypeCostsThatFieldOnly) {

    const auto doc = make_document(
            kvp("name", "billing"),
            kvp("instances", [](bsoncxx::builder::basic::sub_array sa) {
                sa.append(make_document(kvp("instanceId", "inst-1"),
                                        kvp("pid", 100),
                                        kvp("state", bsoncxx::types::b_null{}),
                                        kvp("socketPath", 7),
                                        // The three an instance writes about itself, which the
                                        // manager never writes - a document from before they
                                        // existed simply has none of them.
                                        kvp("utilisation", 61.5),
                                        kvp("backlog", 12)));
            }));

    Module module;
    BOOST_REQUIRE_NO_THROW(module = Module::fromDocument(doc.view()));

    BOOST_REQUIRE(module.instances.size() == 1U);
    const auto &instance = module.instances.front();
    BOOST_TEST(instance.instanceId == "inst-1");
    BOOST_TEST(instance.pid == 100);
    BOOST_TEST(instance.socketPath.empty());
    BOOST_TEST(instance.utilisation == 61.5);
    BOOST_TEST(instance.backlog == 12L);
}

BOOST_AUTO_TEST_CASE(ADocumentWithNothingRecognisableIsStillADocument) {

    const auto doc = make_document(kvp("somethingElse", "entirely"));

    Module module;
    BOOST_REQUIRE_NO_THROW(module = Module::fromDocument(doc.view()));
    BOOST_TEST(module.name.empty());
}
