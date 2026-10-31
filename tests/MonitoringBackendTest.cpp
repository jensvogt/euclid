// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE MonitoringBackendTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/emo/MongoEmoRepository.h>

using Euclid::Database::Database;
using Euclid::Database::MongoEmoRepository;
using Euclid::Database::MonitoringQuery;
using Euclid::Database::Entity::Monitoring::Dashboard;
using Euclid::Database::Entity::Monitoring::MetricType;
using Euclid::Database::Entity::Monitoring::MonitoringData;
using Euclid::Database::Entity::Monitoring::Resolution;

// Monitoring is the one part of euclid not offered on an in-memory installation. Both derived
// figures - the sample-weighted average and the rollup that groups by "$dateTrunc" and "$merge"s a
// coarser tier back into the collection it read - are aggregation pipelines, and the store
// implements documents rather than an aggregation engine. Reimplementing them here would put a
// second implementation of exactly the arithmetic most likely to disagree with the first behind
// the same interface, and metrics that are subtly wrong are worse than metrics that are absent.
//
// What must hold is that "not offered" is a clean answer rather than a failure: the samples still
// go in and still come out, and the rollup - which runs on a timer for the life of the process -
// returns rather than throwing an exception into a log every time it fires.

namespace {

    MonitoringData sample(const std::string &name, const double value, const long samples) {

        MonitoringData data;
        data.name = name;
        data.type = MetricType::GAUGE;
        data.resolution = Resolution::RAW;
        data.timestamp = std::chrono::system_clock::now();
        data.value = value;
        data.samples = samples;
        data.minValue = value;
        data.maxValue = value;
        return data;
    }

}// namespace

BOOST_AUTO_TEST_CASE(SamplesAreStoredAndListedOnTheInMemoryBackend) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    repository.upsert(sample("cpu.usage", 12.5, 3));

    MonitoringQuery query;
    query.resolution = Resolution::RAW;
    query.name = "cpu.usage";

    const auto found = repository.list(query);
    BOOST_TEST_REQUIRE(found.size() == 1U);
    BOOST_TEST(found[0].name == "cpu.usage");
    BOOST_TEST(found[0].value == 12.5);
    BOOST_TEST(found[0].samples == 3);
}

BOOST_AUTO_TEST_CASE(TheAggregatedTiersAnswerRatherThanThrow) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    repository.upsert(sample("cpu.usage", 12.5, 3));

    MonitoringQuery query;
    query.resolution = Resolution::RAW;
    query.name = "cpu.usage";

    // Nothing, because there is no aggregation engine to compute it - not an exception, and not a
    // number arrived at some other way that would disagree with what MongoDB reports for the same
    // samples.
    BOOST_CHECK_NO_THROW(std::ignore = repository.average(query));
    BOOST_TEST(repository.average(query) == 0.0);

    const auto now = std::chrono::system_clock::now();
    BOOST_CHECK_NO_THROW(std::ignore = repository.rollup(Resolution::RAW, Resolution::HOUR, now - std::chrono::hours(1), now,
                                                          std::chrono::hours(24)));
    BOOST_TEST(repository.rollup(Resolution::RAW, Resolution::HOUR, now - std::chrono::hours(1), now, std::chrono::hours(24)) == 0);

    // And the samples it could not roll up are still there to be read.
    BOOST_TEST(repository.list(query).size() == 1U);
}

// ── Label maps ───────────────────────────────────────────────────────────────
// A metric euclid records about itself has one dimension ("method", "module"); a metric an
// application pushes has as many as its meter carried. What has to hold is that the second kind
// survives a round trip, that a query can narrow by one dimension of several, and that two series
// differing only in a dimension stay two series rather than overwriting each other.

BOOST_AUTO_TEST_CASE(EveryDimensionSurvivesTheRoundTrip) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    auto data = sample("jvm.memory.used", 4096, 1);
    data.labels = {{"area", "heap"}, {"id", "G1 Eden Space"}};
    repository.upsert(data);

    MonitoringQuery query;
    query.resolution = Resolution::RAW;
    query.name = "jvm.memory.used";

    const auto found = repository.list(query);
    BOOST_TEST_REQUIRE(found.size() == 1U);
    BOOST_TEST(found[0].labels.size() == 2U);
    BOOST_TEST(found[0].labels.at("area") == "heap");
    BOOST_TEST(found[0].labels.at("id") == "G1 Eden Space");
    // The accessors the one-dimensional readers use: the first pair in key order.
    BOOST_TEST(found[0].labelName() == "area");
    BOOST_TEST(found[0].labelValue() == "heap");
}

BOOST_AUTO_TEST_CASE(AQueryNarrowsByOneDimensionOfSeveral) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    auto heap = sample("jvm.memory.used", 4096, 1);
    heap.labels = {{"area", "heap"}, {"id", "G1 Eden Space"}};
    repository.upsert(heap);

    auto nonHeap = sample("jvm.memory.used", 512, 1);
    nonHeap.labels = {{"area", "nonheap"}, {"id", "Metaspace"}};
    repository.upsert(nonHeap);

    // Two series, not one overwriting the other: they differ only in their labels.
    MonitoringQuery all;
    all.resolution = Resolution::RAW;
    all.name = "jvm.memory.used";
    BOOST_TEST(repository.list(all).size() == 2U);

    MonitoringQuery byArea = all;
    byArea.labels = {{"area", "heap"}};
    const auto heapOnly = repository.list(byArea);
    BOOST_TEST_REQUIRE(heapOnly.size() == 1U);
    BOOST_TEST(heapOnly[0].value == 4096);

    // The single-pair form, which is what the wire format and every older caller send.
    MonitoringQuery byPair = all;
    byPair.labelName = "id";
    byPair.labelValue = "Metaspace";
    const auto metaspace = repository.list(byPair);
    BOOST_TEST_REQUIRE(metaspace.size() == 1U);
    BOOST_TEST(metaspace[0].value == 512);

    // A value with no dimension named - "whichever label carries this" - which is answered off
    // labelKey rather than by unpacking the map, so that it works on this backend too.
    MonitoringQuery byValue = all;
    byValue.labelValue = "nonheap";
    BOOST_TEST(repository.list(byValue).size() == 1U);

    // And a value that is only a prefix of one that is there matches nothing, rather than
    // everything the anchorless pattern would have caught.
    MonitoringQuery byPrefix = all;
    byPrefix.labelValue = "non";
    BOOST_TEST(repository.list(byPrefix).empty());
}

BOOST_AUTO_TEST_CASE(ASeriesIsIdentifiedByItsWholeLabelSet) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    auto first = sample("http.requests", 1, 1);
    first.labels = {{"method", "GET"}, {"status", "200"}};
    repository.upsert(first);

    // Same name, same first dimension, different second: a different series, so upserting it must
    // add a row rather than replace the one above.
    auto second = sample("http.requests", 7, 1);
    second.labels = {{"method", "GET"}, {"status", "500"}};
    second.timestamp = first.timestamp;
    repository.upsert(second);

    MonitoringQuery query;
    query.resolution = Resolution::RAW;
    query.name = "http.requests";
    BOOST_TEST(repository.list(query).size() == 2U);

    // The same series again in the same bucket replaces its row, which is what makes a flush
    // repeatable.
    auto again = second;
    again.value = 9;
    repository.upsert(again);
    BOOST_TEST(repository.list(query).size() == 2U);
}

// ── Saved dashboards ─────────────────────────────────────────────────────────
// Layouts rather than measurements, and the one thing this module stores on a person's behalf.
// What has to hold is that a layout comes back exactly as it went in, that re-saving replaces
// rather than accumulates, and above all that the user id partitions the collection: two users
// keeping an "Overview" each must be two dashboards, and neither must be able to read, overwrite
// or delete the other's by asking for it by name.

namespace {

    Dashboard dashboard(const std::string &userId, const std::string &name, const std::string &document) {

        Dashboard saved;
        saved.userId = userId;
        saved.name = name;
        saved.document = document;
        saved.created = std::chrono::system_clock::now();
        saved.modified = saved.created;
        return saved;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ADashboardComesBackExactlyAsItWasSaved) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    // Keys with dots, a unicode value, a float and a deep nesting - the things that would not
    // survive being stored as BSON, which is why the layout is stored as text.
    const std::string layout = R"({"name":"Overview","panels":[{"id":"p1","w":8.5,"labels":{"euclid.module":"eqs"},"title":"Löwe · 90%"}]})";
    repository.upsertDashboard(dashboard("jvo", "Overview", layout));

    const auto found = repository.getDashboard("jvo", "Overview");
    BOOST_TEST_REQUIRE(found.has_value());
    BOOST_TEST(found->userId == "jvo");
    BOOST_TEST(found->name == "Overview");
    BOOST_TEST(found->document == layout);
}

BOOST_AUTO_TEST_CASE(SavingAgainReplacesRatherThanAccumulates) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    repository.upsertDashboard(dashboard("jvo", "Overview", R"({"panels":[]})"));
    repository.upsertDashboard(dashboard("jvo", "Overview", R"({"panels":[{"id":"p1"}]})"));

    const auto all = repository.listDashboards("jvo");
    BOOST_TEST_REQUIRE(all.size() == 1U);
    BOOST_TEST(all[0].document == R"({"panels":[{"id":"p1"}]})");
}

BOOST_AUTO_TEST_CASE(TwoUsersEachKeepTheirOwnOverview) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    repository.upsertDashboard(dashboard("jvo", "Overview", R"({"owner":"jvo"})"));
    repository.upsertDashboard(dashboard("admin", "Overview", R"({"owner":"admin"})"));

    // Same name, two rows: the pair is the identity, not the name.
    const auto mine = repository.getDashboard("jvo", "Overview");
    const auto theirs = repository.getDashboard("admin", "Overview");
    BOOST_TEST_REQUIRE(mine.has_value());
    BOOST_TEST_REQUIRE(theirs.has_value());
    BOOST_TEST(mine->document == R"({"owner":"jvo"})");
    BOOST_TEST(theirs->document == R"({"owner":"admin"})");

    // And a list is one user's own, not everything stored.
    BOOST_TEST(repository.listDashboards("jvo").size() == 1U);
    BOOST_TEST(repository.listDashboards("admin").size() == 1U);
}

BOOST_AUTO_TEST_CASE(OneUserCannotReachAnothersDashboard) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    repository.upsertDashboard(dashboard("jvo", "Private", R"({"owner":"jvo"})"));

    // Asking for it as somebody else finds nothing rather than finding it by name.
    BOOST_TEST(!repository.getDashboard("admin", "Private").has_value());

    // And deleting it as somebody else removes nothing - the row is still jvo's afterwards.
    BOOST_TEST(repository.deleteDashboard("admin", "Private") == 0);
    BOOST_TEST(repository.getDashboard("jvo", "Private").has_value());
}

BOOST_AUTO_TEST_CASE(AMissingDashboardIsAnAnswerRatherThanAFailure) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    BOOST_TEST(!repository.getDashboard("jvo", "NeverSaved").has_value());
    BOOST_TEST(repository.listDashboards("nobody").empty());
    BOOST_TEST(repository.deleteDashboard("jvo", "NeverSaved") == 0);
}

BOOST_AUTO_TEST_CASE(ADashboardIsRemovedByNameForItsOwner) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    repository.upsertDashboard(dashboard("jvo", "Scratch", R"({"panels":[]})"));
    repository.upsertDashboard(dashboard("jvo", "Keep", R"({"panels":[]})"));

    BOOST_TEST(repository.deleteDashboard("jvo", "Scratch") == 1);
    BOOST_TEST(!repository.getDashboard("jvo", "Scratch").has_value());

    const auto left = repository.listDashboards("jvo");
    BOOST_TEST_REQUIRE(left.size() == 1U);
    BOOST_TEST(left[0].name == "Keep");
}

// Every metric by name, folded from its series: how many there are and which labels tell them
// apart. Answered from the rows on the in-memory backend too, which has no pipeline to group with.
BOOST_AUTO_TEST_CASE(MetricsAreListedByNameWithTheirSeries) {

    Database::instance().initializeMemory();
    MongoEmoRepository repository;

    auto cpuA = sample("system-cpu-usage", 10.0, 1);
    cpuA.labels = {{"host", "a"}};
    auto cpuB = sample("system-cpu-usage", 20.0, 1);
    cpuB.labels = {{"host", "b"}};
    auto load = sample("system-load-average", 1.0, 1);
    load.labels = {{"host", "a"}, {"interval", "1m"}};
    auto old = sample("gone-metric", 1.0, 1);
    old.timestamp -= std::chrono::hours(48);
    for (const auto &row: {cpuA, cpuB, load, old}) repository.upsert(row);

    const auto metrics = repository.listMetrics(std::chrono::system_clock::now() - std::chrono::hours(24));

    BOOST_TEST_REQUIRE(metrics.size() == 2U);
    BOOST_TEST(metrics[0].name == "system-cpu-usage");
    BOOST_TEST(metrics[0].series == 2);
    BOOST_TEST((metrics[0].labelNames == std::vector<std::string>{"host"}));
    BOOST_TEST(metrics[1].name == "system-load-average");
    BOOST_TEST((metrics[1].labelNames == std::vector<std::string>{"host", "interval"}));
}
