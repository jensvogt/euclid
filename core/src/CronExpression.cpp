//
// Created by vogje01 on 17/08/2026.
//

#include <euclid/core/CronExpression.h>

// C++ standard includes
#include <algorithm>
#include <ctime>
#include <stdexcept>

// Boost includes
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

namespace Euclid::Core {

    namespace {

        struct Tm {
            int minute, hour, dayOfMonth, month, dayOfWeek;
        };

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
        _months = ParseField(fields[3], 1, 12);
        _daysOfWeek = ParseField(fields[4], 0, 7);
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
