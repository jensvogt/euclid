// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE AlertEvaluatorTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

// Euclid includes
#include <AlertEvaluator.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/emo/MongoEmoRepository.h>

// So a failed comparison prints the level rather than refusing to compile.
namespace Euclid::Database::Entity::Monitoring {
    inline std::ostream &operator<<(std::ostream &out, const AlertLevel level) { return out << AlertLevelToString(level); }
}// namespace Euclid::Database::Entity::Monitoring

using Euclid::Monitoring::AlertChange;
using Euclid::Monitoring::ClassifyAlertValue;
using Euclid::Monitoring::EvaluateAlert;
using Euclid::Monitoring::MarkAlertNoData;
using Euclid::Monitoring::ValidateAlertRule;
using Euclid::Database::Entity::Monitoring::AlertComparison;
using Euclid::Database::Entity::Monitoring::AlertLevel;
using Euclid::Database::Entity::Monitoring::AlertRule;
using Euclid::Database::Entity::Monitoring::AlertState;
using Euclid::Database::Entity::Monitoring::AlertTransition;
using Euclid::Database::Entity::Monitoring::Resolution;

namespace {

    using Clock = std::chrono::system_clock;
    constexpr auto kPeriod = std::chrono::seconds(300);

    // A load-per-core rule: warning past 0.7, critical past 0.9.
    AlertRule loadRule(const long forPeriods = 1, const long clearPeriods = 1) {
        AlertRule rule;
        rule.accountId = "000000000000";
        rule.ruleId = "worker-load";
        rule.name = "system-load-per-core";
        rule.warning = 0.7;
        rule.critical = 0.9;
        rule.forPeriods = forPeriods;
        rule.clearPeriods = clearPeriods;
        return rule;
    }

    // Feeds one figure per period and returns the levels the state was in after each.
    std::vector<AlertLevel> run(const AlertRule &rule, AlertState &state, const std::vector<double> &values) {
        std::vector<AlertLevel> levels;
        auto bucket = Clock::time_point{} + std::chrono::hours(1000);
        for (const auto value: values) {
            std::ignore = EvaluateAlert(rule, state, value, bucket);
            levels.push_back(state.level);
            bucket += kPeriod;
        }
        return levels;
    }

}// namespace

// Past a line is strictly past it, in whichever direction the rule looks.
BOOST_AUTO_TEST_CASE(AFigureIsClassifiedByTheLinesItIsPast) {

    auto rule = loadRule();
    BOOST_TEST(ClassifyAlertValue(rule, 0.5) == AlertLevel::OK);
    BOOST_TEST(ClassifyAlertValue(rule, 0.7) == AlertLevel::OK);
    BOOST_TEST(ClassifyAlertValue(rule, 0.8) == AlertLevel::WARNING);
    BOOST_TEST(ClassifyAlertValue(rule, 0.9) == AlertLevel::WARNING);
    BOOST_TEST(ClassifyAlertValue(rule, 0.95) == AlertLevel::CRITICAL);

    // Free disk: bad below.
    rule.comparison = AlertComparison::BELOW;
    rule.warning = 20.0;
    rule.critical = 10.0;
    BOOST_TEST(ClassifyAlertValue(rule, 50.0) == AlertLevel::OK);
    BOOST_TEST(ClassifyAlertValue(rule, 15.0) == AlertLevel::WARNING);
    BOOST_TEST(ClassifyAlertValue(rule, 5.0) == AlertLevel::CRITICAL);
}

// One period is a blip; forPeriods of them is a state.
BOOST_AUTO_TEST_CASE(AStateIsEnteredOnlyAfterTheDwell) {

    const auto rule = loadRule(2, 1);
    AlertState state;
    const auto levels = run(rule, state, {0.8, 0.5, 0.8, 0.8});
    BOOST_TEST((levels == std::vector{AlertLevel::OK, AlertLevel::OK, AlertLevel::OK, AlertLevel::WARNING}));
}

// A period past the critical line is past the warning line too: WARNING then CRITICAL with a dwell
// of two is a warning, not nothing.
BOOST_AUTO_TEST_CASE(ABreachThatClimbsCountsTowardsTheWarning) {

    const auto rule = loadRule(2, 1);
    AlertState state;
    const auto levels = run(rule, state, {0.8, 0.95, 0.95});
    BOOST_TEST((levels == std::vector{AlertLevel::OK, AlertLevel::WARNING, AlertLevel::CRITICAL}));
}

// Straight to CRITICAL when the very first breach is past the critical line, and an escalation is a
// change of its own with the line it crossed.
BOOST_AUTO_TEST_CASE(EscalationIsAChange) {

    const auto rule = loadRule();
    AlertState state;
    const auto at = Clock::now();

    auto change = EvaluateAlert(rule, state, 0.8, at);
    BOOST_REQUIRE(change.has_value());
    BOOST_TEST((change->previous == AlertLevel::OK));
    BOOST_TEST((change->level == AlertLevel::WARNING));
    BOOST_TEST(*change->threshold == 0.7);

    change = EvaluateAlert(rule, state, 0.95, at + kPeriod);
    BOOST_REQUIRE(change.has_value());
    BOOST_TEST((change->previous == AlertLevel::WARNING));
    BOOST_TEST((change->level == AlertLevel::CRITICAL));
    BOOST_TEST(*change->threshold == 0.9);
    BOOST_TEST((state.since == at + kPeriod));

    // A hundred more periods of it are no more changes.
    for (int i = 2; i < 100; ++i) BOOST_TEST(!EvaluateAlert(rule, state, 0.95, at + i * kPeriod).has_value());

    AlertState fresh;
    BOOST_TEST((EvaluateAlert(rule, fresh, 0.99, at)->level == AlertLevel::CRITICAL));
}

// Hysteresis: back inside for clearPeriods running before OK, so a value on the line is not an
// event every period.
BOOST_AUTO_TEST_CASE(AStateIsLeftOnlyAfterTheHysteresis) {

    const auto rule = loadRule(1, 3);
    AlertState state;
    const auto levels = run(rule, state, {0.8, 0.5, 0.5, 0.8, 0.5, 0.5, 0.5});
    BOOST_TEST((levels == std::vector{AlertLevel::WARNING, AlertLevel::WARNING, AlertLevel::WARNING, AlertLevel::WARNING,
                                      AlertLevel::WARNING, AlertLevel::WARNING, AlertLevel::OK}));
}

// A value settling between the lines does not stay CRITICAL for ever.
BOOST_AUTO_TEST_CASE(CriticalSettlesBackToWarning) {

    const auto rule = loadRule(1, 2);
    AlertState state;
    const auto levels = run(rule, state, {0.95, 0.8, 0.8, 0.5, 0.5});
    BOOST_TEST((levels == std::vector{AlertLevel::CRITICAL, AlertLevel::CRITICAL, AlertLevel::WARNING, AlertLevel::WARNING, AlertLevel::OK}));
}

// A rule with only a critical line never says WARNING.
BOOST_AUTO_TEST_CASE(ARuleWithOneLineHasOneState) {

    auto rule = loadRule();
    rule.warning.reset();
    AlertState state;
    const auto levels = run(rule, state, {0.8, 0.95, 0.8});
    BOOST_TEST((levels == std::vector{AlertLevel::OK, AlertLevel::CRITICAL, AlertLevel::OK}));
}

// Not a number is not a figure: no breach, no recovery, streaks untouched.
BOOST_AUTO_TEST_CASE(NotANumberCountsAsNothing) {

    const auto rule = loadRule(2, 1);
    AlertState state;
    std::ignore = EvaluateAlert(rule, state, 0.8, Clock::now());
    BOOST_TEST(!EvaluateAlert(rule, state, std::numeric_limits<double>::quiet_NaN(), Clock::now()).has_value());
    BOOST_TEST(state.breachStreak == 1);
    BOOST_TEST((EvaluateAlert(rule, state, 0.8, Clock::now())->level == AlertLevel::WARNING));
}

// NO_DATA once noDataAfterPeriods rows have not come, with half a period's grace; and back once
// rows come again.
BOOST_AUTO_TEST_CASE(ASeriesThatStopsGoesNoData) {

    auto rule = loadRule(1, 2);
    rule.noDataAfterPeriods = 3;
    AlertState state;
    const auto bucket = Clock::now();
    std::ignore = EvaluateAlert(rule, state, 0.95, bucket);

    // Due at bucket + P, + 2P, + 3P, + 4P: three missing is bucket + 4P, plus the grace.
    BOOST_TEST(!MarkAlertNoData(rule, state, bucket + 4 * kPeriod, kPeriod).has_value());
    const auto change = MarkAlertNoData(rule, state, bucket + 4 * kPeriod + kPeriod / 2, kPeriod);
    BOOST_REQUIRE(change.has_value());
    BOOST_TEST((change->previous == AlertLevel::CRITICAL));
    BOOST_TEST((change->level == AlertLevel::NODATA));
    BOOST_TEST(!change->threshold.has_value());

    // Said once.
    BOOST_TEST(!MarkAlertNoData(rule, state, bucket + 100 * kPeriod, kPeriod).has_value());

    // Rows again, inside the lines: OK after the hysteresis.
    BOOST_TEST(!EvaluateAlert(rule, state, 0.5, bucket + 101 * kPeriod).has_value());
    BOOST_TEST((EvaluateAlert(rule, state, 0.5, bucket + 102 * kPeriod)->level == AlertLevel::OK));
}

BOOST_AUTO_TEST_CASE(NoDataIsOnlyForRulesThatAskForIt) {

    const auto rule = loadRule();
    AlertState state;
    std::ignore = EvaluateAlert(rule, state, 0.5, Clock::now());
    BOOST_TEST(!MarkAlertNoData(rule, state, Clock::now() + std::chrono::hours(24), kPeriod).has_value());
}

BOOST_AUTO_TEST_CASE(ARuleIsCheckedBeforeItIsStored) {

    BOOST_TEST(ValidateAlertRule(loadRule()).empty());

    auto rule = loadRule();
    rule.warning = 0.95;
    BOOST_TEST(!ValidateAlertRule(rule).empty());

    rule = loadRule();
    rule.comparison = AlertComparison::BELOW;
    BOOST_TEST(!ValidateAlertRule(rule).empty());
    rule.warning = 20.0;
    rule.critical = 10.0;
    BOOST_TEST(ValidateAlertRule(rule).empty());

    rule = loadRule();
    rule.warning.reset();
    rule.critical.reset();
    BOOST_TEST(!ValidateAlertRule(rule).empty());

    rule = loadRule();
    rule.resolution = Resolution::HOUR;
    BOOST_TEST(!ValidateAlertRule(rule).empty());

    rule = loadRule();
    rule.ruleId = "../x";
    BOOST_TEST(!ValidateAlertRule(rule).empty());

    rule = loadRule();
    rule.forPeriods = 0;
    BOOST_TEST(!ValidateAlertRule(rule).empty());
}

// The selector is equality on a subset: a series may carry more labels than the rule names.
BOOST_AUTO_TEST_CASE(TheSelectorMatchesASubsetOfTheLabels) {

    auto rule = loadRule();
    BOOST_TEST(rule.matches({{"host", "a"}}));
    rule.labels = {{"host", "a"}};
    BOOST_TEST(rule.matches({{"host", "a"}, {"interval", "1m"}}));
    BOOST_TEST(!rule.matches({{"host", "b"}}));
    BOOST_TEST(!rule.matches({}));
}

// Rules, states and history survive the store, on the backend every installation can run.
BOOST_AUTO_TEST_CASE(RulesStatesAndHistoryAreStored) {

    Euclid::Database::Database::instance().initializeMemory();
    Euclid::Database::MongoEmoRepository repository;

    auto rule = loadRule(2, 3);
    rule.labels = {{"host", "pi"}};
    rule.noDataAfterPeriods = 4;
    rule.warning.reset();
    BOOST_REQUIRE(repository.upsertAlertRule(rule));

    const auto stored = repository.getAlertRule(rule.accountId, rule.ruleId);
    BOOST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->name == "system-load-per-core");
    BOOST_TEST(!stored->warning.has_value());
    BOOST_TEST(*stored->critical == 0.9);
    BOOST_TEST(stored->forPeriods == 2);
    BOOST_TEST(stored->clearPeriods == 3);
    BOOST_TEST(stored->noDataAfterPeriods == 4);
    BOOST_TEST(stored->labels.at("host") == "pi");
    BOOST_TEST(repository.listAlertRules("").size() == 1U);
    BOOST_TEST(repository.listAlertRules("someone-else").empty());

    AlertState state;
    state.accountId = rule.accountId;
    state.ruleId = rule.ruleId;
    state.labelKey = "host=pi";
    state.labels = {{"host", "pi"}};
    std::ignore = EvaluateAlert(rule, state, 0.95, Clock::now());
    std::ignore = EvaluateAlert(rule, state, 0.95, Clock::now());
    BOOST_REQUIRE(repository.upsertAlertState(state));
    BOOST_REQUIRE(repository.upsertAlertState(state));

    const auto states = repository.listAlertStates(rule.accountId, rule.ruleId);
    BOOST_REQUIRE(states.size() == 1U);
    BOOST_TEST((states[0].level == AlertLevel::CRITICAL));
    BOOST_TEST(states[0].criticalStreak == 2);

    for (int i = 0; i < 3; ++i) {
        AlertTransition transition;
        transition.accountId = rule.accountId;
        transition.ruleId = rule.ruleId;
        transition.level = AlertLevel::CRITICAL;
        transition.timestamp = Clock::now() + std::chrono::minutes(i);
        transition.expiresAt = transition.timestamp + std::chrono::hours(1);
        BOOST_REQUIRE(repository.insertAlertTransition(transition));
    }
    Euclid::Database::AlertHistoryQuery query;
    query.accountId = rule.accountId;
    query.limit = 2;
    const auto history = repository.listAlertHistory(query);
    BOOST_REQUIRE(history.size() == 2U);
    BOOST_TEST((history[0].timestamp > history[1].timestamp));

    BOOST_TEST(repository.deleteAlertStates(rule.accountId, rule.ruleId) == 1);
    BOOST_TEST(repository.deleteAlertRule(rule.accountId, rule.ruleId) == 1);
    BOOST_TEST(!repository.getAlertRule(rule.accountId, rule.ruleId).has_value());
}
