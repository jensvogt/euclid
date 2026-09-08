#define BOOST_TEST_MODULE DocumentStoreTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <cstdint>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/database/emd/DocumentStore.h>

using Euclid::Database::Emd::DocumentStore;
using Euclid::Database::Emd::FindOptions;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;

// The point of this store is that the repositories cannot tell it from MongoDB. They are written
// against one set of semantics - upsert by a compound key, "$set" with "$setOnInsert" and
// "$currentDate", a "^prefix" regex, sort/skip/limit, a unique index that refuses a second row
// with the same name - and every one of those has to mean here what it means there. Anywhere it
// diverges is a test that passes on this backend and a failure in production, which is worse than
// having no second backend at all.

namespace {

    constexpr auto kCollection = "ess_secret";

    bsoncxx::document::value secret(const std::string &name, const std::string &ns = "development", const std::int64_t version = 1) {
        return make_document(kvp("accountId", "000000000000"), kvp("namespace", ns), kvp("name", name),
                             kvp("value", "ciphertext"), kvp("version", version));
    }

    std::string field(const bsoncxx::document::value &document, const std::string &name) {
        return std::string(document.view()[name].get_string().value);
    }

}// namespace

BOOST_AUTO_TEST_SUITE(DocumentStoreTest)

    BOOST_AUTO_TEST_CASE(AnInsertedDocumentIsFoundByItsFields) {

        DocumentStore store;
        std::ignore = store.InsertOne(kCollection, secret("db-password").view());

        const auto found = store.FindOne(kCollection, make_document(kvp("name", "db-password")).view());
        BOOST_REQUIRE(found.has_value());
        BOOST_CHECK_EQUAL(field(*found, "value"), "ciphertext");

        // Every document gets an id, whether or not the caller supplied one - entities read it
        // back as their oid.
        BOOST_CHECK(found->view()["_id"].type() == bsoncxx::type::k_oid);

        BOOST_CHECK(!store.FindOne(kCollection, make_document(kvp("name", "nothing")).view()).has_value());
        BOOST_CHECK(!store.FindOne("no_such_collection", make_document().view()).has_value());
    }

    BOOST_AUTO_TEST_CASE(UpsertCreatesFromTheFilterThenUpdatesInPlace) {

        DocumentStore store;
        const auto filter = make_document(kvp("accountId", "000000000000"), kvp("namespace", "development"), kvp("name", "db-password"));

        // Exactly the shape every repository upserts with.
        const auto update = make_document(
                kvp("$set", make_document(kvp("value", "first"))),
                kvp("$setOnInsert", make_document(kvp("created", bsoncxx::types::b_date{std::chrono::system_clock::now()}))),
                kvp("$currentDate", make_document(kvp("modified", true))));

        const auto created = store.FindOneAndUpdate(kCollection, filter.view(), update.view(), true, true);
        BOOST_REQUIRE(created.has_value());

        // The created document carries the filter's own fields, which is what lets a repository
        // upsert by name without restating the name in the update.
        BOOST_CHECK_EQUAL(field(*created, "name"), "db-password");
        BOOST_CHECK_EQUAL(field(*created, "accountId"), "000000000000");
        BOOST_CHECK_EQUAL(field(*created, "value"), "first");
        BOOST_CHECK(created->view()["created"].type() == bsoncxx::type::k_date);
        BOOST_CHECK(created->view()["modified"].type() == bsoncxx::type::k_date);

        const auto createdAt = created->view()["created"].get_date().to_int64();
        const auto id = created->view()["_id"].get_oid().value.to_string();

        const auto update2 = make_document(
                kvp("$set", make_document(kvp("value", "second"))),
                kvp("$setOnInsert", make_document(kvp("created", bsoncxx::types::b_date{std::chrono::system_clock::now()}))),
                kvp("$currentDate", make_document(kvp("modified", true))));

        const auto updated = store.FindOneAndUpdate(kCollection, filter.view(), update2.view(), true, true);
        BOOST_REQUIRE(updated.has_value());

        BOOST_CHECK_EQUAL(field(*updated, "value"), "second");
        BOOST_CHECK_EQUAL(store.CountDocuments(kCollection, make_document().view()), 1);

        // The second write is the same document: same id, and the creation date it was inserted
        // with rather than the one the update carried - that is what $setOnInsert is for, and
        // getting it wrong would reset every row's age on every write.
        BOOST_CHECK_EQUAL(updated->view()["_id"].get_oid().value.to_string(), id);
        BOOST_CHECK_EQUAL(updated->view()["created"].get_date().to_int64(), createdAt);
    }

    BOOST_AUTO_TEST_CASE(ReturnBeforeGivesTheDocumentAsItWas) {

        DocumentStore store;
        std::ignore = store.InsertOne(kCollection, secret("db-password").view());

        const auto before = store.FindOneAndUpdate(kCollection, make_document(kvp("name", "db-password")).view(),
                                                   make_document(kvp("$set", make_document(kvp("value", "second")))).view(), false, false);
        BOOST_REQUIRE(before.has_value());
        BOOST_CHECK_EQUAL(field(*before, "value"), "ciphertext");
        BOOST_CHECK_EQUAL(field(*store.FindOne(kCollection, make_document(kvp("name", "db-password")).view()), "value"), "second");
    }

    BOOST_AUTO_TEST_CASE(APrefixRegexMatchesTheWayAListingNeeds) {

        DocumentStore store;
        std::ignore = store.InsertOne(kCollection, secret("db-password").view());
        std::ignore = store.InsertOne(kCollection, secret("db-user").view());
        std::ignore = store.InsertOne(kCollection, secret("api-token").view());

        const auto filter = make_document(kvp("name", make_document(kvp("$regex", "^db-"))));
        BOOST_CHECK_EQUAL(store.CountDocuments(kCollection, filter.view()), 2);
        BOOST_CHECK_EQUAL(store.Find(kCollection, filter.view()).size(), 2U);

        // Anchored, so a prefix filter does not quietly behave like a substring search.
        BOOST_CHECK_EQUAL(store.CountDocuments(kCollection, make_document(kvp("name", make_document(kvp("$regex", "^token")))).view()), 0);
    }

    BOOST_AUTO_TEST_CASE(SortSkipAndLimitPageTheWayAListingDoes) {

        DocumentStore store;
        for (const auto &name: {"c-secret", "a-secret", "d-secret", "b-secret"}) {
            std::ignore = store.InsertOne(kCollection, secret(name).view());
        }

        FindOptions options;
        options.sort = make_document(kvp("name", 1));
        options.limit = 2;
        options.skip = 1;

        const auto page = store.Find(kCollection, make_document().view(), options);
        BOOST_REQUIRE_EQUAL(page.size(), 2U);
        BOOST_CHECK_EQUAL(field(page[0], "name"), "b-secret");
        BOOST_CHECK_EQUAL(field(page[1], "name"), "c-secret");

        options.sort = make_document(kvp("name", -1));
        options.skip = 0;
        const auto descending = store.Find(kCollection, make_document().view(), options);
        BOOST_CHECK_EQUAL(field(descending[0], "name"), "d-secret");
    }

    BOOST_AUTO_TEST_CASE(AUniqueIndexRefusesASecondRowWithTheSameKey) {

        DocumentStore store;
        store.CreateIndex(kCollection, make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)).view(), true);

        std::ignore = store.InsertOne(kCollection, secret("db-password").view());

        // The same name in the same namespace is the duplicate the index exists to refuse - a
        // store that accepted it would let a test pass that production would reject.
        BOOST_CHECK_THROW(std::ignore = store.InsertOne(kCollection, secret("db-password").view()), std::runtime_error);

        // The same name in another namespace is a different resource, which is the whole reason
        // the index is compound.
        BOOST_CHECK_NO_THROW(std::ignore = store.InsertOne(kCollection, secret("db-password", "production").view()));
        BOOST_CHECK_EQUAL(store.CountDocuments(kCollection, make_document().view()), 2);

        // And an upsert of the row that already exists is not a duplicate of itself.
        BOOST_CHECK_NO_THROW(std::ignore = store.FindOneAndUpdate(
                                     kCollection,
                                     make_document(kvp("accountId", "000000000000"), kvp("namespace", "development"), kvp("name", "db-password")).view(),
                                     make_document(kvp("$set", make_document(kvp("value", "second")))).view(), true, true));
        BOOST_CHECK_EQUAL(store.CountDocuments(kCollection, make_document().view()), 2);
    }

    BOOST_AUTO_TEST_CASE(ComparisonOperatorsWorkAcrossNumericWidths) {

        DocumentStore store;
        std::ignore = store.InsertOne("emo_data", make_document(kvp("name", "cpu"), kvp("value", static_cast<std::int64_t>(10))).view());
        std::ignore = store.InsertOne("emo_data", make_document(kvp("name", "cpu"), kvp("value", 20.5)).view());
        std::ignore = store.InsertOne("emo_data", make_document(kvp("name", "mem"), kvp("value", 30)).view());

        BOOST_CHECK_EQUAL(store.CountDocuments("emo_data", make_document(kvp("value", make_document(kvp("$gte", 20)))).view()), 2);
        BOOST_CHECK_EQUAL(store.CountDocuments("emo_data", make_document(kvp("value", make_document(kvp("$lt", 20)))).view()), 1);
        BOOST_CHECK_EQUAL(store.CountDocuments("emo_data", make_document(kvp("name", make_document(kvp("$ne", "cpu")))).view()), 1);
        BOOST_CHECK_EQUAL(store.CountDocuments("emo_data", make_document(kvp("name", make_document(kvp("$in", make_array("cpu", "disk"))))).view()), 2);
        BOOST_CHECK_EQUAL(store.CountDocuments("emo_data", make_document(kvp("missing", make_document(kvp("$exists", false)))).view()), 3);

        // An int64 10 and a double 10.0 are the same number, whichever way round they were
        // written - entities store longs, aggregations produce doubles.
        BOOST_CHECK_EQUAL(store.CountDocuments("emo_data", make_document(kvp("value", 10.0)).view()), 1);
    }

    BOOST_AUTO_TEST_CASE(ADottedPathReachesIntoAnArrayOfSubdocuments) {

        DocumentStore store;
        std::ignore = store.InsertOne("eam_user", make_document(
                                                          kvp("userId", "jens"),
                                                          kvp("accessKeys", make_array(make_document(kvp("accessKeyId", "AKIA1"), kvp("active", true)),
                                                                                       make_document(kvp("accessKeyId", "AKIA2"), kvp("active", false)))))
                                                          .view());

        // How a user is found by one of their keys - the filter EAM signs every request with.
        const auto found = store.FindOne("eam_user", make_document(kvp("accessKeys.accessKeyId", "AKIA2")).view());
        BOOST_REQUIRE(found.has_value());
        BOOST_CHECK_EQUAL(field(*found, "userId"), "jens");

        BOOST_CHECK(!store.FindOne("eam_user", make_document(kvp("accessKeys.accessKeyId", "AKIA9")).view()).has_value());
    }

    BOOST_AUTO_TEST_CASE(IncrementsAddUpAndDeletesRemoveWhatTheyMatch) {

        DocumentStore store;
        std::ignore = store.InsertOne("eqs_queue", make_document(kvp("name", "orders"), kvp("available", static_cast<std::int64_t>(2))).view());

        // How a queue's counters move as messages arrive and leave.
        std::ignore = store.UpdateOne("eqs_queue", make_document(kvp("name", "orders")).view(),
                                      make_document(kvp("$inc", make_document(kvp("available", 3)))).view());
        std::ignore = store.UpdateOne("eqs_queue", make_document(kvp("name", "orders")).view(),
                                      make_document(kvp("$inc", make_document(kvp("available", -1)))).view());

        const auto queue = store.FindOne("eqs_queue", make_document(kvp("name", "orders")).view());
        BOOST_REQUIRE(queue.has_value());
        BOOST_CHECK_EQUAL(queue->view()["available"].get_int64().value, 4);

        // A counter that was never there starts from zero rather than being skipped.
        std::ignore = store.UpdateOne("eqs_queue", make_document(kvp("name", "orders")).view(),
                                      make_document(kvp("$inc", make_document(kvp("delayed", 2)))).view());
        BOOST_CHECK_EQUAL(store.FindOne("eqs_queue", make_document(kvp("name", "orders")).view())->view()["delayed"].get_int64().value, 2);

        BOOST_CHECK_EQUAL(store.Delete("eqs_queue", make_document(kvp("name", "nothing")).view()), 0);
        BOOST_CHECK_EQUAL(store.Delete("eqs_queue", make_document(kvp("name", "orders")).view()), 1);
        BOOST_CHECK_EQUAL(store.CountDocuments("eqs_queue", make_document().view()), 0);
    }

    BOOST_AUTO_TEST_CASE(DeleteManyTakesEveryMatchAndFindOneAndDeleteTakesOne) {

        DocumentStore store;
        for (int i = 0; i < 3; ++i) {
            std::ignore = store.InsertOne("eqs_message", make_document(kvp("queueErn", "orders"), kvp("body", std::to_string(i))).view());
        }
        std::ignore = store.InsertOne("eqs_message", make_document(kvp("queueErn", "other"), kvp("body", "x")).view());

        const auto taken = store.FindOneAndDelete("eqs_message", make_document(kvp("queueErn", "orders")).view());
        BOOST_REQUIRE(taken.has_value());
        BOOST_CHECK_EQUAL(field(*taken, "body"), "0");

        // Purging a queue leaves every other queue's messages alone.
        BOOST_CHECK_EQUAL(store.Delete("eqs_message", make_document(kvp("queueErn", "orders")).view()), 2);
        BOOST_CHECK_EQUAL(store.CountDocuments("eqs_message", make_document().view()), 1);
    }

    BOOST_AUTO_TEST_CASE(GroupCountAnswersTheRecountsTheRepositoriesAskFor) {

        DocumentStore store;
        const auto message = [](const std::string &queue, const std::string &status, const std::int64_t size) {
            return make_document(kvp("queueErn", queue), kvp("status", status), kvp("size", size));
        };
        std::ignore = store.InsertOne("eqs_message", message("orders", "AVAILABLE", 100).view());
        std::ignore = store.InsertOne("eqs_message", message("orders", "AVAILABLE", 50).view());
        std::ignore = store.InsertOne("eqs_message", message("orders", "INVISIBLE", 20).view());
        std::ignore = store.InsertOne("eqs_message", message("other", "AVAILABLE", 7).view());

        // "How many messages and how many bytes, per queue and status" - the shape EQS, ENS and
        // ESM each recount with, and the only aggregation this store implements.
        const auto groups = store.GroupCount("eqs_message", make_document().view(), {"queueErn", "status"}, "size");
        BOOST_REQUIRE_EQUAL(groups.size(), 3U);

        const auto find = [&groups](const std::string &queue, const std::string &status) {
            return std::ranges::find_if(groups, [&](const auto &g) { return g.key[0] == queue && g.key[1] == status; });
        };
        BOOST_REQUIRE(find("orders", "AVAILABLE") != groups.end());
        BOOST_CHECK_EQUAL(find("orders", "AVAILABLE")->count, 2);
        BOOST_CHECK_EQUAL(find("orders", "AVAILABLE")->sum, 150);
        BOOST_CHECK_EQUAL(find("orders", "INVISIBLE")->count, 1);
        BOOST_CHECK_EQUAL(find("other", "AVAILABLE")->sum, 7);
    }

    BOOST_AUTO_TEST_CASE(AnOperatorTheStoreDoesNotKnowIsRefusedRatherThanIgnored) {

        DocumentStore store;
        std::ignore = store.InsertOne(kCollection, secret("db-password").view());

        // The failure mode that matters: a filter or an update this store silently did nothing
        // with would read as "no rows matched" or "saved", and the caller would believe it.
        BOOST_CHECK_THROW(std::ignore = store.CountDocuments(kCollection, make_document(kvp("name", make_document(kvp("$mod", make_array(2, 0))))).view()),
                          std::runtime_error);
        BOOST_CHECK_THROW(std::ignore = store.UpdateOne(kCollection, make_document(kvp("name", "db-password")).view(),
                                                        make_document(kvp("$rename", make_document(kvp("name", "other")))).view()),
                          std::runtime_error);

        // Including an update given as a plain document, which in MongoDB would replace the whole
        // row - accepting it here as a no-op would lose the difference.
        BOOST_CHECK_THROW(std::ignore = store.UpdateOne(kCollection, make_document(kvp("name", "db-password")).view(),
                                                        make_document(kvp("value", "replaced")).view()),
                          std::runtime_error);
    }

    BOOST_AUTO_TEST_CASE(NestedDocumentsAndArraysSurviveAnUpsert) {

        DocumentStore store;

        // What an entity actually looks like: strings and numbers, but also a tag map and a list.
        // These go through the update machinery, which rebuilds the document field by field, and a
        // field that only *looked* copied would come back as garbage - or as nothing at all.
        const auto entity = make_document(
                kvp("accountId", "000000000000"), kvp("namespace", "development"), kvp("name", "ess"),
                kvp("algorithm", "AES"), kvp("length", static_cast<std::int64_t>(256)),
                kvp("tags", make_document(kvp("system", "payroll"), kvp("owner", "jens"))),
                kvp("subjectAltNames", make_array("DNS:localhost", "IP:127.0.0.1")));

        const auto filter = make_document(kvp("accountId", "000000000000"), kvp("namespace", "development"), kvp("name", "ess"));
        const auto update = make_document(kvp("$set", entity),
                                          kvp("$currentDate", make_document(kvp("modified", true))));

        const auto created = store.FindOneAndUpdate("ekm_key", filter.view(), update.view(), true, true);
        BOOST_REQUIRE(created.has_value());

        const auto view = created->view();
        BOOST_CHECK_EQUAL(std::string(view["algorithm"].get_string().value), "AES");

        BOOST_REQUIRE(view["tags"].type() == bsoncxx::type::k_document);
        BOOST_CHECK_EQUAL(std::string(view["tags"].get_document().value["system"].get_string().value), "payroll");
        BOOST_CHECK_EQUAL(std::string(view["tags"].get_document().value["owner"].get_string().value), "jens");

        BOOST_REQUIRE(view["subjectAltNames"].type() == bsoncxx::type::k_array);
        BOOST_CHECK_EQUAL(std::string(view["subjectAltNames"].get_array().value.find(0)->get_string().value), "DNS:localhost");

        // And again when it is read back rather than returned from the write.
        const auto found = store.FindOne("ekm_key", filter.view());
        BOOST_REQUIRE(found.has_value());
        BOOST_CHECK_EQUAL(std::string(found->view()["tags"].get_document().value["system"].get_string().value), "payroll");

        // A second upsert of the same row must not read as a duplicate of itself, with or without
        // a unique index over the fields the filter names.
        store.CreateIndex("ekm_key", make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)).view(), true);
        BOOST_CHECK_NO_THROW(std::ignore = store.FindOneAndUpdate("ekm_key", filter.view(), update.view(), true, true));
        BOOST_CHECK_EQUAL(store.CountDocuments("ekm_key", make_document().view()), 1);
    }

    BOOST_AUTO_TEST_CASE(AnEmptyStringIsAValueAndStaysOne) {

        DocumentStore store;

        // An unscoped namespace is the empty string, and euclid writes it constantly. Rebuilding a
        // document through the driver's own value type turns one into a null, which is worse than
        // losing it: the row is stored, reports success, and can never be found again - the filter
        // that would find it looks for a string.
        const std::string unscoped;
        const auto filter = make_document(kvp("accountId", "000000000000"), kvp("namespace", unscoped), kvp("name", "ess"));
        const auto update = make_document(kvp("$set", make_document(kvp("accountId", "000000000000"),
                                                                    kvp("namespace", unscoped),
                                                                    kvp("name", "ess"),
                                                                    kvp("description", unscoped))));

        const auto created = store.FindOneAndUpdate("ekm_key", filter.view(), update.view(), true, true);
        BOOST_REQUIRE(created.has_value());
        BOOST_CHECK(created->view()["namespace"].type() == bsoncxx::type::k_string);
        BOOST_CHECK_EQUAL(std::string(created->view()["namespace"].get_string().value), "");
        BOOST_CHECK(created->view()["description"].type() == bsoncxx::type::k_string);

        // And the row the write reported is the row the next lookup finds.
        BOOST_REQUIRE(store.FindOne("ekm_key", filter.view()).has_value());
        BOOST_CHECK_EQUAL(store.CountDocuments("ekm_key", filter.view()), 1);

        // Which is also what keeps a second upsert from reading as a duplicate of a row it cannot
        // see.
        store.CreateIndex("ekm_key", make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)).view(), true);
        BOOST_CHECK_NO_THROW(std::ignore = store.FindOneAndUpdate("ekm_key", filter.view(), update.view(), true, true));
        BOOST_CHECK_EQUAL(store.CountDocuments("ekm_key", make_document().view()), 1);

        // An insert takes the same path through the rebuild.
        std::ignore = store.InsertOne("ekm_key", make_document(kvp("namespace", unscoped), kvp("name", "other")).view());
        BOOST_CHECK_EQUAL(store.CountDocuments("ekm_key", make_document(kvp("namespace", unscoped)).view()), 2);
    }

    BOOST_AUTO_TEST_CASE(ThePositionalOperatorUpdatesTheElementTheFilterMatched) {

        DocumentStore store;

        // How EMM records what is running: one document per module, one array element per instance,
        // updated in place on every state change. Without the positional operator the update fails
        // and an installation cannot report its own instances.
        std::ignore = store.InsertOne("emm_module", make_document(
                                                            kvp("name", "esm"),
                                                            kvp("instances", bsoncxx::builder::basic::make_array(
                                                                                     make_document(kvp("instanceId", "a"), kvp("state", "STARTING"), kvp("pid", 100)),
                                                                                     make_document(kvp("instanceId", "b"), kvp("state", "STARTING"), kvp("pid", 200)))))
                                                            .view());

        const auto filter = make_document(kvp("name", "esm"), kvp("instances.instanceId", "b"));
        const auto update = make_document(kvp("$set", make_document(
                                                              kvp("instances.$", make_document(kvp("instanceId", "b"), kvp("state", "RUNNING"), kvp("pid", 201))))));

        const auto result = store.UpdateOne("emm_module", filter.view(), update.view());
        BOOST_TEST(result.matched == 1);
        BOOST_TEST(result.modified == 1);

        const auto found = store.FindOne("emm_module", make_document(kvp("name", "esm")).view());
        BOOST_REQUIRE(found.has_value());
        const auto instances = found->view()["instances"].get_array().value;

        // The matched element changed and only that one: an update that rewrote the whole array
        // would take the other instance's record with it.
        BOOST_CHECK_EQUAL(std::string(instances.find(0)->get_document().value["state"].get_string().value), "STARTING");
        BOOST_CHECK_EQUAL(instances.find(0)->get_document().value["pid"].get_int32().value, 100);
        BOOST_CHECK_EQUAL(std::string(instances.find(1)->get_document().value["state"].get_string().value), "RUNNING");
        BOOST_CHECK_EQUAL(instances.find(1)->get_document().value["pid"].get_int32().value, 201);

        // One field of the matched element rather than the whole of it.
        const auto single = make_document(kvp("$set", make_document(kvp("instances.$.state", "STOPPED"))));
        std::ignore = store.UpdateOne("emm_module", filter.view(), single.view());

        const auto after = store.FindOne("emm_module", make_document(kvp("name", "esm")).view());
        const auto updated = after->view()["instances"].get_array().value;
        BOOST_CHECK_EQUAL(std::string(updated.find(1)->get_document().value["state"].get_string().value), "STOPPED");
        BOOST_CHECK_EQUAL(updated.find(1)->get_document().value["pid"].get_int32().value, 201);

        // A filter that matches no element updates nothing, rather than the zeroth - which is what
        // lets EMM fall back to $push for an instance it has not recorded yet.
        const auto absent = make_document(kvp("name", "esm"), kvp("instances.instanceId", "zzz"));
        BOOST_TEST(store.UpdateOne("emm_module", absent.view(), single.view()).matched == 0);
        BOOST_CHECK_EQUAL(std::string(store.FindOne("emm_module", make_document(kvp("name", "esm")).view())
                                              ->view()["instances"]
                                              .get_array()
                                              .value.find(0)
                                              ->get_document()
                                              .value["state"]
                                              .get_string()
                                              .value),
                          "STARTING");
    }

BOOST_AUTO_TEST_SUITE_END()
