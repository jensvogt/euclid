// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EmmExportImportTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <map>
#include <string>
#include <tuple>

// Mongo includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/oid.hpp>

// Euclid includes
#include <ExportImport.h>
#include <euclid/database/Database.h>

using Euclid::Database::Database;
using Euclid::EMM::ExportCollection;
using Euclid::EMM::ImportCollection;

// An export of one euclid and an import into another is how a deployment is moved, and euclid-mgr
// in a container runs on the in-memory store rather than on MongoDB. These two used to reach for
// Database::client() - the connection pool - which a process on that store never created, so
// importing a perfectly good ens.json failed with "MongoDB not initialized - call initialize()
// first" before a single document was read. What is pinned here is that both go through
// Database::collection(), which answers on whichever backend the process was pointed at.

namespace {

    namespace basic = bsoncxx::builder::basic;

    constexpr auto kCollection = "ens_topic";

    void freshDatabase() {
        Database::instance().initializeMemory();
    }

    // An ObjectId, as every euclid document carries: the store keys on one, and Extended JSON
    // renders it as {"$oid": "..."} - which is what a real export file holds.
    bsoncxx::oid insertTopic(const std::string &name) {
        const bsoncxx::oid id;
        Database::instance().collection(kCollection).insert_one(
                basic::make_document(basic::kvp("_id", id), basic::kvp("name", name), basic::kvp("retentionPeriod", 1209600)));
        return id;
    }

    boost::json::array exported() {
        return ExportCollection(Database::instance().collection(kCollection));
    }

    Euclid::EMM::ImportOutcome importInto(const boost::json::array &docs) {
        return ImportCollection(Database::instance().collection(kCollection), kCollection, docs);
    }

    // The documents of a collection, by "_id", so a test can say what is there without depending on
    // the order the store returns them in.
    std::map<std::string, boost::json::object> byId(const boost::json::array &docs) {
        std::map<std::string, boost::json::object> result;
        for (const auto &doc: docs) {
            result[std::string(doc.at("_id").at("$oid").as_string())] = doc.as_object();
        }
        return result;
    }

}// namespace

// The trap itself, stated once: on the in-memory store there is no pool, so anything that asks for
// one throws - while the collection the same process is meant to use answers normally.
BOOST_AUTO_TEST_CASE(TheInMemoryStoreHasNoConnectionPool) {

    freshDatabase();

    BOOST_CHECK_THROW(std::ignore = Database::instance().client(), std::runtime_error);
    BOOST_CHECK_NO_THROW(std::ignore = Database::instance().collection(kCollection).count_documents({}));
}

BOOST_AUTO_TEST_CASE(ExportReadsTheCollectionOnTheInMemoryStore) {

    freshDatabase();
    const auto orders = insertTopic("order-events");
    const auto audit = insertTopic("audit");

    const auto docs = exported();

    BOOST_REQUIRE(docs.size() == 2U);
    const auto topics = byId(docs);
    BOOST_TEST(topics.at(orders.to_string()).at("name").as_string() == "order-events");
    BOOST_TEST(topics.at(audit.to_string()).at("name").as_string() == "audit");
}

BOOST_AUTO_TEST_CASE(ExportOfAnEmptyCollectionIsAnEmptyArray) {

    freshDatabase();

    BOOST_TEST(exported().empty());
}

// The round trip a file makes: out of one installation, into another that has never seen it. The
// ids come across too - a restored document that was given a new one would no longer be the
// document the rest of the export refers to.
BOOST_AUTO_TEST_CASE(ImportWritesWhatExportWrote) {

    freshDatabase();
    const auto orders = insertTopic("order-events");
    const auto audit = insertTopic("audit");
    const auto file = exported();

    freshDatabase();
    const auto outcome = importInto(file);

    BOOST_TEST(outcome.imported == 2L);
    BOOST_TEST(outcome.failed == 0L);

    const auto topics = byId(exported());
    BOOST_REQUIRE(topics.size() == 2U);
    BOOST_TEST(topics.at(orders.to_string()).at("name").as_string() == "order-events");
    BOOST_TEST(topics.at(audit.to_string()).at("retentionPeriod").as_int64() == 1209600);
}

// Upsert by "_id", not insert: importing the same file twice leaves one copy of each document,
// which is what makes re-running a restore safe.
BOOST_AUTO_TEST_CASE(ImportingTheSameFileTwiceIsIdempotent) {

    freshDatabase();
    std::ignore = insertTopic("order-events");
    const auto file = exported();

    freshDatabase();
    std::ignore = importInto(file);
    const auto outcome = importInto(file);

    BOOST_TEST(outcome.imported == 1L);
    BOOST_TEST(exported().size() == 1U);
}

// The whole document is replaced rather than merged, so a restore reflects the file exactly -
// including a field the live document has since gained.
BOOST_AUTO_TEST_CASE(ImportReplacesRatherThanMerges) {

    freshDatabase();
    const auto orders = insertTopic("order-events");
    const auto file = exported();

    freshDatabase();
    Database::instance().collection(kCollection).insert_one(
            basic::make_document(basic::kvp("_id", orders), basic::kvp("name", "renamed"), basic::kvp("since", "added later")));

    std::ignore = importInto(file);

    const auto topics = byId(exported());
    BOOST_REQUIRE(topics.size() == 1U);
    BOOST_TEST(topics.at(orders.to_string()).at("name").as_string() == "order-events");
    BOOST_TEST(!topics.at(orders.to_string()).contains("since"));
}

// A file is external input and may have been hand-edited, so one unusable document is counted and
// stepped over rather than abandoning the rest of the collection.
BOOST_AUTO_TEST_CASE(AnUnusableDocumentFailsOnItsOwn) {

    freshDatabase();

    const boost::json::array file{
            boost::json::object{{"_id", {{"$oid", bsoncxx::oid{}.to_string()}}}, {"name", "order-events"}},
            // No "_id": there is nothing to upsert on, and inserting anyway would make a second copy
            // of it on every re-import.
            boost::json::object{{"name", "no-id"}},
            boost::json::object{{"_id", {{"$oid", bsoncxx::oid{}.to_string()}}}, {"name", "audit"}},
    };

    const auto outcome = importInto(file);

    BOOST_TEST(outcome.imported == 2L);
    BOOST_TEST(outcome.failed == 1L);
    BOOST_TEST(exported().size() == 2U);
}
