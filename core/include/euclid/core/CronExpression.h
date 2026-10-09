// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 17/08/2026.
//

#pragma once

// C++ standard includes
#include <chrono>
#include <set>
#include <string>
#include <vector>

namespace Euclid::Core {

    using std::chrono::system_clock;

    /**
     * @brief Parses a standard 5-field cron expression and computes occurrences.
     *
     * @par
     * Supported field syntax: '*', a single value, a range 'a-b', a step '*\/n' or 'a-b/n',
     * and comma separated lists of any of the above, e.g. '0,15,30,45 * * * *'.
     *
     * @par
     * Field order (matching Unix cron): minute (0-59) hour (0-23) day-of-month (1-31) month (1-12)
     * day-of-week (0-6, 0 and 7 both mean Sunday). If both day-of-month and day-of-week are
     * restricted (not '*'), a time point matches when either field matches, per the POSIX cron rule.
     *
     * @par
     * The month and day-of-week fields also accept the usual three-letter names, in any case and in
     * any of the forms above: 'MON-FRI', 'SAT,SUN', '0 0 1 JAN,JUL *'.
     *
     * @par
     * The aliases '\@yearly', '\@annually', '\@monthly', '\@weekly', '\@daily', '\@midnight' and
     * '\@hourly' are also accepted in place of the five fields.
     *
     * @par
     * All calculations are done in UTC.
     *
     * @author jensvogt47\@gmail.com
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
         * @brief Rewrites the three-letter names in a field into the numbers they stand for.
         *
         * @par
         * Applied to the month and day-of-week fields before they are parsed, which is what lets
         * 'MON-FRI' go on being a range and 'SAT,SUN' a list: the names are substituted in place,
         * so every form ParseField() understands keeps working with names in it.
         *
         * @param field field text, e.g. 'MON-FRI'
         * @param names the three-letter names in the order the field numbers them
         * @param base the value the first name stands for - 0 for day-of-week, 1 for month
         * @return the field with every name replaced by its number
         * @throws std::invalid_argument if the field contains a word that is not one of the names
         */
        static std::string ResolveNames(const std::string &field, const std::vector<std::string> &names, int base);

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

    /**
     * @brief What to do with a scheduled thing when its stored moment is compared with the clock.
     */
    enum class CronRelease {

        /**
         * @brief The moment has not come. Nothing to do.
         */
        NotDue,

        /**
         * @brief There is a schedule but no moment stored against it, so one has to be computed
         * before anything can be due.
         *
         * @par
         * Distinct from NotDue because it is the one case that has to write something down, and
         * distinct from Fire because the alternative - reading the epoch as a moment that passed -
         * means a nightly job starts the instant the scheduler comes up, every time it comes up.
         */
        Undated,

        /**
         * @brief The moment has come and the previous run has not finished, so this one is skipped.
         */
        Skip,

        /**
         * @brief The moment has come. Run it.
         */
        Fire
    };

    /**
     * @brief Decides what a stored occurrence means now.
     *
     * @par
     * The whole of a cron scheduler's behaviour that is not cron arithmetic, in one place and with
     * no clock of its own, so that it can be stated as a table and tested as one. Whoever calls it
     * supplies the time, advances the occurrence and does the work.
     *
     * @param nextRunAt the moment stored against the schedule; the epoch if there is none
     * @param now the time to judge it against
     * @param running whether the previous run is still in flight
     * @return what to do - see CronRelease
     */
    [[nodiscard]]
    constexpr CronRelease CronReleaseFor(const system_clock::time_point &nextRunAt, const system_clock::time_point &now, const bool running) {

        if (nextRunAt.time_since_epoch().count() == 0) return CronRelease::Undated;
        if (nextRunAt > now) return CronRelease::NotDue;
        return running ? CronRelease::Skip : CronRelease::Fire;
    }

}// namespace Euclid::Core
