// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 17/08/2026.
//

#include <euclid/core/CronExpression.h>

// C++ standard includes
#include <algorithm>
#include <cctype>
#include <ctime>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

// Boost includes
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

namespace Euclid::Core {

    namespace {

        struct Tm {
            int minute, hour, dayOfMonth, month, dayOfWeek;
        };

        // Three-letter names in the order cron numbers them, so a name's value is its index plus the
        // field's own base: Sunday is 0 in the day-of-week field, January is 1 in the month field.
        const std::vector<std::string> kDayNames{"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        const std::vector<std::string> kMonthNames{"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

        Tm ToTm(const system_clock::time_point &timePoint) {
            const time_t timeT = system_clock::to_time_t(timePoint);
            struct tm tm {};
#ifdef _WIN32
            gmtime_s(&tm, &timeT);
#else
            gmtime_r(&timeT, &tm);
#endif
            return {tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon + 1, tm.tm_wday};
        }

    }// namespace

    std::string CronExpression::ResolveAlias(const std::string &expression) {
        std::string trimmed = boost::algorithm::trim_copy(expression);
        if (trimmed == "@yearly" || trimmed == "@annually") return "0 0 1 1 *";
        if (trimmed == "@monthly") return "0 0 1 * *";
        if (trimmed == "@weekly") return "0 0 * * 0";
        if (trimmed == "@daily" || trimmed == "@midnight") return "0 0 * * *";
        if (trimmed == "@hourly") return "0 * * * *";
        return trimmed;
    }

    std::string CronExpression::ResolveNames(const std::string &field, const std::vector<std::string> &names, const int base) {

        // Substitution rather than parsing, so every form ParseField() already understands keeps
        // working with names in it: "MON-FRI" becomes "1-5", "SAT,SUN" becomes "6,0", and
        // "MON-FRI/2" becomes "1-5/2" without this function knowing what a range or a step is.
        std::string result;
        result.reserve(field.size());

        for (std::size_t i = 0; i < field.size();) {

            if (!std::isalpha(static_cast<unsigned char>(field[i]))) {
                result += field[i];
                ++i;
                continue;
            }

            std::size_t end = i;
            while (end < field.size() && std::isalpha(static_cast<unsigned char>(field[end]))) ++end;

            auto word = field.substr(i, end - i);
            std::ranges::transform(word, word.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });

            const auto found = std::ranges::find(names, word);
            if (found == names.end()) throw std::invalid_argument("Invalid cron name in field: " + field);

            result += std::to_string(base + static_cast<int>(std::distance(names.begin(), found)));
            i = end;
        }

        return result;
    }

    std::set<int> CronExpression::ParseField(const std::string &field, const int min, const int max) {
        std::set<int> result;

        std::vector<std::string> parts;
        boost::algorithm::split(parts, field, boost::algorithm::is_any_of(","));

        for (const auto &part: parts) {
            std::string rangePart = part;
            int step = 1;

            if (const auto slashPos = part.find('/'); slashPos != std::string::npos) {
                rangePart = part.substr(0, slashPos);
                step = std::stoi(part.substr(slashPos + 1));
                if (step <= 0) throw std::invalid_argument("Invalid cron step in field: " + field);
            }

            int rangeStart = min, rangeEnd = max;
            if (rangePart != "*") {
                if (const auto dashPos = rangePart.find('-'); dashPos != std::string::npos) {
                    rangeStart = std::stoi(rangePart.substr(0, dashPos));
                    rangeEnd = std::stoi(rangePart.substr(dashPos + 1));
                } else {
                    rangeStart = rangeEnd = std::stoi(rangePart);
                }
            }

            if (rangeStart < min || rangeEnd > max || rangeStart > rangeEnd) throw std::invalid_argument("Invalid cron field: " + field);

            for (int v = rangeStart; v <= rangeEnd; v += step) result.insert(v);
        }

        if (result.empty()) throw std::invalid_argument("Invalid cron field: " + field);
        return result;
    }

    CronExpression::CronExpression(const std::string &expression) : _expression(expression) {
        const std::string resolved = ResolveAlias(expression);

        std::vector<std::string> fields;
        boost::algorithm::split(fields, resolved, boost::algorithm::is_any_of(" \t"), boost::algorithm::token_compress_on);
        fields.erase(std::remove_if(fields.begin(), fields.end(), [](const std::string &f) { return f.empty(); }), fields.end());

        if (fields.size() != 5) throw std::invalid_argument("Cron expression must have 5 fields: '" + expression + "'");

        _minutes = ParseField(fields[0], 0, 59);
        _hours = ParseField(fields[1], 0, 23);
        _daysOfMonth = ParseField(fields[2], 1, 31);
        _months = ParseField(ResolveNames(fields[3], kMonthNames, 1), 1, 12);
        _daysOfWeek = ParseField(ResolveNames(fields[4], kDayNames, 0), 0, 7);
        if (_daysOfWeek.contains(7)) {
            _daysOfWeek.erase(7);
            _daysOfWeek.insert(0);
        }

        _dayOfMonthRestricted = boost::algorithm::trim_copy(fields[2]) != "*";
        _dayOfWeekRestricted = boost::algorithm::trim_copy(fields[4]) != "*";
    }

    system_clock::time_point CronExpression::Next(const system_clock::time_point &from) const {
        using namespace std::chrono;

        // Start at the next whole minute strictly after 'from'.
        auto candidate = time_point_cast<minutes>(from) + minutes(1);

        // Bound the search so an impossible expression (e.g. day-of-month 31 in February only) does not loop forever.
        constexpr int maxMinutesToScan = 4 * 366 * 24 * 60;
        for (int i = 0; i < maxMinutesToScan; ++i) {
            const auto tm = ToTm(candidate);

            bool dayMatches;
            if (_dayOfMonthRestricted && _dayOfWeekRestricted) {
                dayMatches = _daysOfMonth.contains(tm.dayOfMonth) || _daysOfWeek.contains(tm.dayOfWeek);
            } else if (_dayOfMonthRestricted) {
                dayMatches = _daysOfMonth.contains(tm.dayOfMonth);
            } else if (_dayOfWeekRestricted) {
                dayMatches = _daysOfWeek.contains(tm.dayOfWeek);
            } else {
                dayMatches = true;
            }

            if (_months.contains(tm.month) && dayMatches && _hours.contains(tm.hour) && _minutes.contains(tm.minute)) return candidate;

            candidate += minutes(1);
        }

        throw std::runtime_error("Cron expression '" + _expression + "' has no occurrence in the next four years");
    }

}// namespace Euclid::Core
