//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::Monitoring {

    /**
     * @brief Kind of quantity a monitoring metric measures.
     *
     * @par
     * The distinction decides how samples are combined, both when the monitoring module averages a
     * collection period and when one resolution tier is rolled up into the next: a RATE is summed
     * (twelve five-minute call totals make one hourly call total), a GAUGE is averaged (twelve
     * five-minute mean service times make one hourly mean service time). Storing this on the row is
     * what makes the rollup possible at all - without it a rollup reading a persisted value back
     * cannot tell whether summing or averaging is correct.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class MetricType {
        GAUGE,
        RATE,
        UNKNOWN
    };

    static std::map<MetricType, std::string> MetricTypeNames{
            {MetricType::GAUGE, "GAUGE"},
            {MetricType::RATE, "RATE"},
            {MetricType::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string MetricTypeToString(const MetricType &metricType) {
        return MetricTypeNames[metricType];
    }

    [[maybe_unused]]
    static MetricType MetricTypeFromString(const std::string &metricType) {
        const auto it = std::ranges::find_if(MetricTypeNames, [&metricType](const auto &pair) { return pair.second == metricType; });
        return it != MetricTypeNames.end() ? it->first : MetricType::UNKNOWN;
    }

}// namespace Euclid::Database::Entity::Monitoring
