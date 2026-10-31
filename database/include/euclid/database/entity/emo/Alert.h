// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/emo/Resolution.h>

namespace Euclid::Database::Entity::Monitoring {

    /**
     * @brief Which of a row's three figures a rule judges.
     *
     * @par
     * An EMO row carries value, minValue and maxValue, and value is a period's mean for a GAUGE and
     * its sum for a RATE. A rule on the mean of a service time never sees a thirty-second stall
     * inside a five-minute bucket; a rule on the max does. So the choice is the rule's, stated,
     * rather than implied by which series somebody happened to point it at.
     */
    enum class AlertStatistic {
        VALUE,
        MAX,
        MIN,
        UNKNOWN
    };

    /**
     * @brief Which side of a threshold is the wrong one.
     *
     * @par
     * Both are needed: queue depth and latency are bad above, free disk and a heartbeat rate below.
     */
    enum class AlertComparison {
        ABOVE,
        BELOW,
        UNKNOWN
    };

    /**
     * @brief Where one series stands against its rule.
     *
     * @par
     * NODATA rather than NO_DATA because <netdb.h> defines NO_DATA as a macro, and anything that
     * includes Boost.Asio has it. Stored and shown as "NO_DATA" all the same.
     *
     * @par
     * NODATA is a state of its own rather than a kind of OK: a series that stopped arriving is
     * exactly what somebody watching it wants to hear about, and silence otherwise looks the same
     * as health.
     */
    enum class AlertLevel {
        OK,
        WARNING,
        CRITICAL,
        NODATA,
        UNKNOWN
    };

    [[nodiscard]] std::string AlertStatisticToString(AlertStatistic statistic);
    [[nodiscard]] AlertStatistic AlertStatisticFromString(const std::string &statistic);
    [[nodiscard]] std::string AlertComparisonToString(AlertComparison comparison);
    [[nodiscard]] AlertComparison AlertComparisonFromString(const std::string &comparison);
    [[nodiscard]] std::string AlertLevelToString(AlertLevel level);
    [[nodiscard]] AlertLevel AlertLevelFromString(const std::string &level);

    /**
     * @brief The lower-case word an event and a client use for a level: "ok", "warning",
     * "critical", "nodata".
     *
     * @param level a level
     * @return its severity
     */
    [[nodiscard]] std::string AlertSeverity(AlertLevel level);

    /**
     * @brief A threshold rule on one metric.
     *
     * @par
     * Name, a label selector, which figure, which direction, two numbers and how long to wait - and
     * deliberately nothing more. A rule language is a query engine, and nobody asked for one.
     *
     * @par Identity
     * accountId plus ruleId. The rule belongs to the account that wrote it, which is what its
     * events carry for a subscriber to filter on; the metrics it reads are the installation's and
     * have no account.
     */
    struct AlertRule {

        /**
         * @brief Database id, empty until it has been written.
         */
        std::string oid;

        /**
         * @brief Account the rule belongs to.
         */
        std::string accountId;

        /**
         * @brief Namespace it was written in, carried on its events.
         */
        std::string nameSpace;

        /**
         * @brief The rule's own name, unique in its account: "worker-load".
         */
        std::string ruleId;

        /**
         * @brief The metric it judges: "system-load-per-core".
         */
        std::string name;

        /**
         * @brief Equality on a subset of a row's labels. Empty matches every series of the metric,
         * each with a state of its own.
         */
        std::map<std::string, std::string> labels;

        /**
         * @brief The tier it reads. RAW only for now - a rule on HOUR or DAY is a report.
         */
        Resolution resolution{Resolution::RAW};

        /**
         * @brief Which figure of a row it judges.
         */
        AlertStatistic statistic{AlertStatistic::VALUE};

        /**
         * @brief Which side of the thresholds is the wrong one.
         */
        AlertComparison comparison{AlertComparison::ABOVE};

        /**
         * @brief The warning threshold, if the rule has one.
         */
        std::optional<double> warning;

        /**
         * @brief The critical threshold, if the rule has one. At least one of the two is set.
         */
        std::optional<double> critical;

        /**
         * @brief Consecutive periods past a threshold before the state is entered.
         */
        long forPeriods{1};

        /**
         * @brief Consecutive periods back inside before it is left. The hysteresis that stops a
         * metric sitting on the line from publishing an event every period for ever.
         */
        long clearPeriods{1};

        /**
         * @brief Periods without a row before a series goes NO_DATA; 0 never.
         */
        long noDataAfterPeriods{0};

        /**
         * @brief A disabled rule is kept but not evaluated.
         */
        bool enabled{true};

        /**
         * @brief What the rule is for, in words.
         */
        std::string description;

        /**
         * @brief When the rule was created.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief When it was last changed.
         */
        std::chrono::system_clock::time_point modified;

        /**
         * @brief Whether a series carrying these labels is one the rule judges: every label the
         * rule names is there with the same value. A series may carry more.
         *
         * @param seriesLabels a row's labels
         * @return true if the selector matches
         */
        [[nodiscard]]
        bool matches(const std::map<std::string, std::string> &seriesLabels) const;

        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        [[nodiscard]]
        static AlertRule fromDocument(const std::optional<bsoncxx::document::view> &doc);
    };

    /**
     * @brief Where one series matched by one rule stands, and how long it has been heading
     * somewhere else.
     *
     * @par
     * Kept in the database rather than in memory, so a restarted EMO carries on from it instead of
     * re-raising everything it finds. One row per rule and series: a rule with no selector on a
     * metric labelled per instance has one per instance.
     *
     * @par The streaks
     * Counted per period, and what the dwell and the hysteresis are decided from - see
     * EvaluateAlert(). Each is the number of consecutive periods, ending with the last one, in which
     * the value was past the warning line (or the critical one where there is no warning),
     * past the critical line, inside every line, and short of the critical line.
     */
    struct AlertState {

        /**
         * @brief Database id, empty until it has been written.
         */
        std::string oid;

        /**
         * @brief Account of the rule.
         */
        std::string accountId;

        /**
         * @brief The rule.
         */
        std::string ruleId;

        /**
         * @brief Which series - see MonitoringData::LabelKey().
         */
        std::string labelKey;

        /**
         * @brief The series' labels, for whoever reads the state.
         */
        std::map<std::string, std::string> labels;

        /**
         * @brief The state it is in.
         */
        AlertLevel level{AlertLevel::OK};

        /**
         * @brief When it entered that state.
         */
        std::chrono::system_clock::time_point since;

        /**
         * @brief The last figure judged.
         */
        double lastValue{};

        /**
         * @brief The bucket of the last row judged. What the no-data sweep measures from.
         */
        std::chrono::system_clock::time_point lastSeen;

        long breachStreak{};
        long criticalStreak{};
        long okStreak{};
        long belowCriticalStreak{};

        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        [[nodiscard]]
        static AlertState fromDocument(const std::optional<bsoncxx::document::view> &doc);
    };

    /**
     * @brief One change of state, kept so a timeline can be drawn and paged back through.
     *
     * @par
     * The event bus is delivery, not an archive: an event is gone once its subscribers have taken
     * it. This is the record, and it expires on its own expiresAt the way a metric row does.
     */
    struct AlertTransition {

        std::string oid;
        std::string accountId;
        std::string nameSpace;
        std::string ruleId;

        /**
         * @brief The metric.
         */
        std::string name;

        std::map<std::string, std::string> labels;
        std::string labelKey;

        /**
         * @brief The state it left.
         */
        AlertLevel previous{AlertLevel::OK};

        /**
         * @brief The state it entered.
         */
        AlertLevel level{AlertLevel::OK};

        /**
         * @brief The figure that decided it; meaningless for NO_DATA.
         */
        double value{};

        /**
         * @brief The line it crossed, if it crossed one.
         */
        std::optional<double> threshold;

        AlertStatistic statistic{AlertStatistic::VALUE};

        /**
         * @brief Why, when it was not a value: "rule-deleted", "rule-disabled". Empty otherwise.
         */
        std::string reason;

        /**
         * @brief When it happened.
         */
        std::chrono::system_clock::time_point timestamp;

        /**
         * @brief When the row is removed by the TTL index.
         */
        std::chrono::system_clock::time_point expiresAt;

        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        [[nodiscard]]
        static AlertTransition fromDocument(const std::optional<bsoncxx::document::view> &doc);
    };

}// namespace Euclid::Database::Entity::Monitoring
