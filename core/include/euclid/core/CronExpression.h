//
// Created by vogje01 on 17/08/2026.
//

#pragma once

// C++ standard includes
#include <chrono>
#include <set>
#include <string>

namespace Euclid::Core {

    using std::chrono::system_clock;

    /**
     * @brief Parses a standard 5-field cron expression and computes occurrences.
     *
     * @par
     * Supported field syntax: '*', a single value, a range 'a-b', a step '*\/n' or 'a-b/n',
     * and comma separated lists of any of the above, e.g. 'MON-FRI', '0,15,30,45 * * * *'.
     *
     * @par
     * Field order (matching Unix cron): minute (0-59) hour (0-23) day-of-month (1-31) month (1-12)
     * day-of-week (0-6, 0 and 7 both mean Sunday). If both day-of-month and day-of-week are
     * restricted (not '*'), a time point matches when either field matches, per the POSIX cron rule.
     *
     * @par
     * The aliases '\@yearly', '\@annually', '\@monthly', '\@weekly', '\@daily', '\@midnight' and
     * '\@hourly' are also accepted in place of the five fields.
     *
     * @par
     * All calculations are done in UTC.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class CronExpression {

    public:

        /**
         * @brief Constructor
         *
         * @param expression cron expression, either five whitespace separated fields or one of the '@' aliases
         * @throws std::invalid_argument if the expression cannot be parsed
         */
        explicit CronExpression(const std::string &expression);

        /**
         * @brief Returns the next occurrence strictly after the given time point.
         *
         * @param from reference time point (exclusive)
         * @return next matching time point, truncated to whole minutes
         * @throws std::runtime_error if no matching time point is found within four years
         */
        [[nodiscard]]
        system_clock::time_point Next(const system_clock::time_point &from) const;

        /**
         * @brief Returns the original expression this instance was constructed from.
         *
         * @return cron expression string
         */
        [[nodiscard]]
        const std::string &Expression() const { return _expression; }

    private:

        /**
         * @brief Parses a single cron field into the set of matching values.
         *
         * @param field field text, e.g. '*\/15' or '1-5'
         * @param min smallest allowed value for this field
         * @param max largest allowed value for this field
         * @return set of matching values in [min, max]
         */
        static std::set<int> ParseField(const std::string &field, int min, int max);

        /**
         * @brief Rewrites a named alias ('\@daily', ...) into the equivalent five field expression.
         *
         * @param expression expression as passed to the constructor
         * @return equivalent five field cron expression
         */
        static std::string ResolveAlias(const std::string &expression);

        /**
         * @brief Minutes (0-59) on which the expression fires.
         */
        std::set<int> _minutes;

        /**
         * @brief Hours (0-23) on which the expression fires.
         */
        std::set<int> _hours;

        /**
         * @brief Days of month (1-31) on which the expression fires.
         */
        std::set<int> _daysOfMonth;

        /**
         * @brief Months (1-12) on which the expression fires.
         */
        std::set<int> _months;

        /**
         * @brief Days of week (0-6, 0 = Sunday) on which the expression fires.
         */
        std::set<int> _daysOfWeek;

        /**
         * @brief True if the day-of-month field was restricted (not '*') in the original expression.
         */
        bool _dayOfMonthRestricted = false;

        /**
         * @brief True if the day-of-week field was restricted (not '*') in the original expression.
         */
        bool _dayOfWeekRestricted = false;

        /**
         * @brief Original expression this instance was constructed from.
         */
        std::string _expression;
    };

}// namespace Euclid::Core
