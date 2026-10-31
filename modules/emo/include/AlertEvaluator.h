// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/entity/emo/Alert.h>
#include <euclid/database/entity/emo/MonitoringData.h>

namespace Euclid::Monitoring {

    using Database::Entity::Monitoring::AlertComparison;
    using Database::Entity::Monitoring::AlertLevel;
    using Database::Entity::Monitoring::AlertRule;
    using Database::Entity::Monitoring::AlertState;
    using Database::Entity::Monitoring::AlertStatistic;

    /**
     * @brief The figure of a row a rule judges - see AlertStatistic.
     *
     * @param rule the rule
     * @param row  a row of the metric it names
     * @return the row's mean or sum, its max, or its min
     */
    inline double AlertFigure(const AlertRule &rule, const Database::Entity::Monitoring::MonitoringData &row) {
        switch (rule.statistic) {
            case AlertStatistic::MAX:
                return row.maxValue;
            case AlertStatistic::MIN:
                return row.minValue;
            default:
                return row.value;
        }
    }

    /**
     * @brief Which band a figure is in: OK, WARNING or CRITICAL. Past a line is strictly past it -
     * a load of exactly 0.9 against a critical line of 0.9 is not critical.
     *
     * @param rule  the rule
     * @param value the figure
     * @return the band, never NODATA
     */
    inline AlertLevel ClassifyAlertValue(const AlertRule &rule, const double value) {
        const auto past = [&](const double threshold) {
            return rule.comparison == AlertComparison::BELOW ? value < threshold : value > threshold;
        };
        if (rule.critical && past(*rule.critical)) return AlertLevel::CRITICAL;
        if (rule.warning && past(*rule.warning)) return AlertLevel::WARNING;
        return AlertLevel::OK;
    }

    /**
     * @brief One change of a series' state, as the evaluator found it.
     */
    struct AlertChange {

        /**
         * @brief The state it left.
         */
        AlertLevel previous{AlertLevel::OK};

        /**
         * @brief The state it entered.
         */
        AlertLevel level{AlertLevel::OK};

        /**
         * @brief The figure that decided it; the last one seen, for NODATA.
         */
        double value{};

        /**
         * @brief The line that was crossed: the one entered, or for a return to OK the one it came
         * back inside of. Nothing for NODATA, and for a return from NODATA.
         */
        std::optional<double> threshold;
    };

    /**
     * @brief Takes one series one period on: counts the period into the streaks and moves the
     * state when a dwell or a hysteresis has been served.
     *
     * @par
     * Pure, so the whole of what alerting decides can be tested without a database: the caller
     * reads the state, hands it here with the period's figure, and writes it back.
     *
     * @par The rules
     * - From OK or NODATA: CRITICAL once past the critical line forPeriods periods running;
     *   otherwise WARNING once past either line that long - a period past the critical line is
     *   past the warning one too. Back from NODATA to OK after clearPeriods periods inside.
     * - From WARNING: CRITICAL as from OK; OK once inside clearPeriods periods running.
     * - From CRITICAL: OK once inside clearPeriods periods running; WARNING once short of the
     *   critical line that long and in the warning band now. Without the second a value that
     *   settles between the lines would stay CRITICAL for ever.
     *
     * @par
     * A figure that is not a number counts as nothing: it is not a breach, it is not a recovery,
     * and it leaves the streaks as they were.
     *
     * @param rule   the rule
     * @param state  the series' state, updated in place
     * @param value  the period's figure - see AlertFigure()
     * @param bucket the bucket the figure belongs to
     * @return the change, if the state changed
     */
    inline std::optional<AlertChange> EvaluateAlert(const AlertRule &rule, AlertState &state, const double value,
                                                    const std::chrono::system_clock::time_point bucket) {

        if (!std::isfinite(value)) return std::nullopt;

        const auto observed = ClassifyAlertValue(rule, value);
        state.breachStreak = observed != AlertLevel::OK ? state.breachStreak + 1 : 0;
        state.criticalStreak = observed == AlertLevel::CRITICAL ? state.criticalStreak + 1 : 0;
        state.okStreak = observed == AlertLevel::OK ? state.okStreak + 1 : 0;
        state.belowCriticalStreak = observed != AlertLevel::CRITICAL ? state.belowCriticalStreak + 1 : 0;
        state.lastValue = value;
        state.lastSeen = bucket;

        const auto dwell = std::max(1L, rule.forPeriods);
        const auto clear = std::max(1L, rule.clearPeriods);

        auto next = state.level;
        switch (state.level) {
            case AlertLevel::WARNING:
                if (state.criticalStreak >= dwell) next = AlertLevel::CRITICAL;
                else if (state.okStreak >= clear) next = AlertLevel::OK;
                break;
            case AlertLevel::CRITICAL:
                if (state.okStreak >= clear) next = AlertLevel::OK;
                else if (state.belowCriticalStreak >= clear && observed == AlertLevel::WARNING) next = AlertLevel::WARNING;
                break;
            default:
                if (state.criticalStreak >= dwell) next = AlertLevel::CRITICAL;
                else if (state.breachStreak >= dwell) next = rule.warning ? AlertLevel::WARNING : state.level;
                else if (state.level != AlertLevel::OK && state.okStreak >= clear) next = AlertLevel::OK;
                break;
        }
        if (next == state.level) return std::nullopt;

        AlertChange change{.previous = state.level, .level = next, .value = value};
        if (next == AlertLevel::CRITICAL) change.threshold = rule.critical;
        else if (next == AlertLevel::WARNING) change.threshold = rule.warning;
        else if (state.level == AlertLevel::CRITICAL) change.threshold = rule.critical;
        else if (state.level == AlertLevel::WARNING) change.threshold = rule.warning;

        state.level = next;
        state.since = bucket;
        return change;
    }

    /**
     * @brief Moves a series to NODATA if its rule asks for that and its rows have stopped.
     *
     * @par
     * A row for bucket B is written as B's period ends, so with a period P the row for B is due at
     * B + P and the n-th after it at B + (n + 1)P. The series is NODATA once noDataAfterPeriods of
     * them have not come, plus half a period's grace so that a flush running a little late is not
     * mistaken for a silence.
     *
     * @param rule   the rule
     * @param state  the series' state, updated in place
     * @param now    the time now
     * @param period the averaging period rows are written at
     * @return the change, if the state changed
     */
    inline std::optional<AlertChange> MarkAlertNoData(const AlertRule &rule, AlertState &state, const std::chrono::system_clock::time_point now,
                                                      const std::chrono::seconds period) {

        if (rule.noDataAfterPeriods <= 0 || state.level == AlertLevel::NODATA) return std::nullopt;
        if (state.lastSeen == std::chrono::system_clock::time_point{}) return std::nullopt;

        if (const auto deadline = state.lastSeen + period * (rule.noDataAfterPeriods + 1) + period / 2; now < deadline) return std::nullopt;

        AlertChange change{.previous = state.level, .level = AlertLevel::NODATA, .value = state.lastValue};
        state.level = AlertLevel::NODATA;
        state.since = now;
        state.breachStreak = state.criticalStreak = state.okStreak = state.belowCriticalStreak = 0;
        return change;
    }

    /**
     * @brief What is wrong with a rule, in words; empty if nothing is.
     *
     * @par
     * Checked before a rule is stored rather than discovered while it is evaluated, where nobody
     * would hear about it.
     *
     * @param rule the rule
     * @return the first problem, or empty
     */
    inline std::string ValidateAlertRule(const AlertRule &rule) {

        if (rule.ruleId.empty()) return R"("ruleId" is required)";
        if (rule.ruleId.size() > 128) return R"("ruleId" is longer than 128 characters)";
        if (!std::ranges::all_of(rule.ruleId, [](const char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            })) {
            return R"("ruleId" may hold letters, digits, '-', '_' and '.')";
        }
        if (rule.name.empty()) return R"("name", the metric, is required)";
        if (rule.resolution != Database::Entity::Monitoring::Resolution::RAW) {
            return R"("resolution" must be RAW: a rule on an hourly or daily tier is a report, not an alert)";
        }
        if (rule.statistic == AlertStatistic::UNKNOWN) return R"("statistic" must be value, max or min)";
        if (rule.comparison == AlertComparison::UNKNOWN) return R"("comparison" must be above or below)";
        if (!rule.warning && !rule.critical) return R"(at least one of "warning" and "critical" is required)";
        if ((rule.warning && !std::isfinite(*rule.warning)) || (rule.critical && !std::isfinite(*rule.critical))) {
            return "thresholds must be finite numbers";
        }
        if (rule.warning && rule.critical) {
            if (rule.comparison == AlertComparison::ABOVE && !(*rule.warning < *rule.critical)) {
                return R"("warning" must be below "critical" for a rule alerting above them)";
            }
            if (rule.comparison == AlertComparison::BELOW && !(*rule.warning > *rule.critical)) {
                return R"("warning" must be above "critical" for a rule alerting below them)";
            }
        }
        if (rule.forPeriods < 1 || rule.forPeriods > 1000) return R"("forPeriods" must be between 1 and 1000)";
        if (rule.clearPeriods < 1 || rule.clearPeriods > 1000) return R"("clearPeriods" must be between 1 and 1000)";
        if (rule.noDataAfterPeriods < 0 || rule.noDataAfterPeriods > 1000) return R"("noDataAfterPeriods" must be between 0 and 1000)";
        return {};
    }

}// namespace Euclid::Monitoring
