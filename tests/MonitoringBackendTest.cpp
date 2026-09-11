// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE MonitoringBackendTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/emo/MongoEmoRepository.h>

using Euclid::Database::Database;
using Euclid::Database::MongoEmoRepository;
using Euclid::Database::MonitoringQuery;
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
