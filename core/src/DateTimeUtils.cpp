// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 30/05/2023.
//

#include <euclid/core/DateTimeUtils.h>

#include <cctype>
#include <cstdio>
#include <string_view>

#ifdef _WIN32
#include <clocale>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#endif

namespace Euclid::Core {

#ifdef _WIN32
    extern "C" char *strptime(const char *s, const char *f, struct tm *tm) {
        // std::get_time uses the same format parameters as strptime.
        std::istringstream input(s);
        input.imbue(std::locale(setlocale(LC_ALL, nullptr)));
        input >> std::get_time(tm, f);
        if (input.fail()) {
            return nullptr;
        }
        return (char *) (s + input.tellg());
    }
#endif

    std::string DateTimeUtils::ToISO8601(const system_clock::time_point &timePoint) {
        return std::format("{:%FT%TZ}", timePoint);
    }

    // Parsed by hand and as UTC, which is what ToISO8601 writes. What this replaced read the string
    // as *local* time through mktime() and then added an hour - right only on a host in CET in
    // winter, an hour out in summer, more elsewhere - and on Windows parsed nothing at all, because
    // the strptime shim above sits on std::get_time, which does not know %F or %T. Lease deadlines
    // and credential expiries go through here, so "an hour early" meant a worker that thought every
    // lease it held had already run out.
    //
    // Accepts "YYYY-MM-DDTHH:MM:SS", optional fractional seconds of any length, then "Z", a
    // "+HH:MM"/"-HH:MM" offset, or nothing - which is taken as UTC, since nothing euclid writes
    // omits the zone. Answers the epoch for a string it cannot read, as the old code effectively did.
    system_clock::time_point DateTimeUtils::FromISO8601(const std::string &dateString) {

        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        int consumed = 0;
        if (std::sscanf(dateString.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute, &second, &consumed) != 6) {
            return system_clock::time_point{};
        }

        const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                                               std::chrono::day{static_cast<unsigned>(day)}};
        if (!date.ok()) return system_clock::time_point{};

        auto point = std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};

        auto rest = std::string_view(dateString).substr(static_cast<std::size_t>(consumed));

        // Fractional seconds, to whatever precision the writer used: MSVC formats system_clock in
        // 100ns ticks, libstdc++ in nanoseconds. Kept to nanoseconds.
        std::chrono::nanoseconds fraction{0};
        if (!rest.empty() && (rest.front() == '.' || rest.front() == ',')) {
            rest.remove_prefix(1);
            long long scale = 100000000;
            while (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest.front()))) {
                fraction += std::chrono::nanoseconds{(rest.front() - '0') * scale};
                scale /= 10;
                rest.remove_prefix(1);
            }
        }

        // The zone: a time written as 14:00+02:00 is 12:00 UTC.
        std::chrono::minutes offset{0};
        if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
            int offsetHours = 0, offsetMinutes = 0;
            if (std::sscanf(std::string(rest.substr(1)).c_str(), "%2d:%2d", &offsetHours, &offsetMinutes) >= 1) {
                offset = std::chrono::hours{offsetHours} + std::chrono::minutes{offsetMinutes};
                if (rest.front() == '-') offset = -offset;
            }
        }

        return std::chrono::time_point_cast<system_clock::duration>(point - offset + fraction);
    }

    system_clock::time_point DateTimeUtils::FromUnixTimestamp(const long timestamp) {
        const system_clock::time_point tp{std::chrono::milliseconds{timestamp}};
#if __APPLE__
        return tp + std::chrono::hours(1);
#else
        return std::chrono::zoned_time{std::chrono::current_zone(), tp + std::chrono::seconds(UtcOffset())};
#endif
    }

#ifdef _WIN32
    system_clock::time_point DateTimeUtils::FromUnixTimestamp(const long long timestamp) {
        const system_clock::time_point tp{std::chrono::milliseconds{timestamp}};
        return std::chrono::zoned_time{std::chrono::current_zone(), tp + std::chrono::seconds(UtcOffset())};
    }
#endif

    std::string DateTimeUtils::HttpFormatNow() {
        return HttpFormat(system_clock::now());
    }

    std::string DateTimeUtils::HttpFormat(const system_clock::time_point &timePoint) {
        char buf[256];
        const time_t timeT = system_clock::to_time_t(timePoint);
        struct tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &timeT);
#else
        gmtime_r(&timeT, &tm);
#endif
        strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
        return {buf};
    }

    long DateTimeUtils::UnixTimestamp(const system_clock::time_point &timePoint) {
        return std::chrono::duration_cast<std::chrono::seconds>(timePoint.time_since_epoch()).count();
    }

    long DateTimeUtils::UnixTimestampMs(const system_clock::time_point &timePoint) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(timePoint.time_since_epoch()).count();
    }

    system_clock::time_point DateTimeUtils::LocalDateTimeNow() {
#if __APPLE__
        return std::chrono::system_clock::now();
#else
        return system_clock::time_point(std::chrono::zoned_time(std::chrono::current_zone(), system_clock::now()).get_local_time().time_since_epoch());
#endif
    }

    system_clock::time_point DateTimeUtils::UtcDateTimeNow() {
#if __APPLE__
        return system_clock::time_point{std::chrono::time_point_cast<std::chrono::milliseconds>(system_clock::now())};
#else
        return system_clock::now();
#endif
    }

    long DateTimeUtils::UtcOffset() {
#if __APPLE__
        time_t clock;
        const tm *localTime = localtime(&clock);
        return localTime->tm_gmtoff;
#else
        return std::chrono::current_zone()->get_info(system_clock::now()).offset.count();
#endif
    }

    system_clock::time_point DateTimeUtils::ConvertToUtc(const system_clock::time_point &value) {
        const long seconds = std::chrono::time_point_cast<std::chrono::seconds>(value).time_since_epoch().count();
        return system_clock::from_time_t(seconds - UtcOffset());
    }

    int DateTimeUtils::GetSecondsUntilMidnight() {
        using namespace std;
        using namespace std::chrono;

        const auto now = system_clock::now();
        const auto today = floor<days>(now);
        return 24 * 3600 - static_cast<int>(duration_cast<seconds>(now - today).count());
    }

} // namespace Euclid::Core
