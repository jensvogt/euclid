// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <cstdint>

#include <euclid/database/entity/emo/Alert.h>

namespace Euclid::Database::Entity::Monitoring {

    namespace {

        using bsoncxx::builder::basic::kvp;

        bsoncxx::types::b_date toDate(const std::chrono::system_clock::time_point &timestamp) {
            return bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch())};
        }

        std::chrono::system_clock::time_point fromDate(const bsoncxx::document::element &field) {
            return field.type() == bsoncxx::type::k_date ? std::chrono::system_clock::time_point{field.get_date().value}
                                                         : std::chrono::system_clock::time_point{};
        }

        // Whichever number type the field was written as: a document edited by hand, or imported
        // through emm, may carry 2 where this module wrote 2.0.
        std::optional<double> number(const bsoncxx::document::element &field) {
            switch (field.type()) {
                case bsoncxx::type::k_double:
                    return field.get_double().value;
                case bsoncxx::type::k_int32:
                    return static_cast<double>(field.get_int32().value);
                case bsoncxx::type::k_int64:
                    return static_cast<double>(field.get_int64().value);
                default:
                    return std::nullopt;
            }
        }

        long integer(const bsoncxx::document::element &field) {
            return static_cast<long>(number(field).value_or(0));
        }

        std::string text(const bsoncxx::document::element &field) {
            return field.type() == bsoncxx::type::k_string ? std::string(field.get_string().value) : std::string();
        }

        bsoncxx::document::value labelDocument(const std::map<std::string, std::string> &labels) {
            bsoncxx::builder::basic::document document{};
            for (const auto &[name, value]: labels) document.append(kvp(name, value));
            return document.extract();
        }

        std::map<std::string, std::string> labelMap(const bsoncxx::document::element &field) {
            std::map<std::string, std::string> labels;
            if (field.type() != bsoncxx::type::k_document) return labels;
            for (const auto &label: field.get_document().value) {
                if (label.type() == bsoncxx::type::k_string) labels[std::string(label.key())] = std::string(label.get_string().value);
            }
            return labels;
        }

        std::string oidOf(const bsoncxx::document::element &field) {
            return field.type() == bsoncxx::type::k_oid ? field.get_oid().value.to_string() : std::string();
        }

    }// namespace

    std::string AlertStatisticToString(const AlertStatistic statistic) {
        switch (statistic) {
            case AlertStatistic::VALUE:
                return "value";
            case AlertStatistic::MAX:
                return "max";
            case AlertStatistic::MIN:
                return "min";
            default:
                return "unknown";
        }
    }

    AlertStatistic AlertStatisticFromString(const std::string &statistic) {
        if (statistic == "value") return AlertStatistic::VALUE;
        if (statistic == "max") return AlertStatistic::MAX;
        if (statistic == "min") return AlertStatistic::MIN;
        return AlertStatistic::UNKNOWN;
    }

    std::string AlertComparisonToString(const AlertComparison comparison) {
        switch (comparison) {
            case AlertComparison::ABOVE:
                return "above";
            case AlertComparison::BELOW:
                return "below";
            default:
                return "unknown";
        }
    }

    AlertComparison AlertComparisonFromString(const std::string &comparison) {
        if (comparison == "above") return AlertComparison::ABOVE;
        if (comparison == "below") return AlertComparison::BELOW;
        return AlertComparison::UNKNOWN;
    }

    std::string AlertLevelToString(const AlertLevel level) {
        switch (level) {
            case AlertLevel::OK:
                return "OK";
            case AlertLevel::WARNING:
                return "WARNING";
            case AlertLevel::CRITICAL:
                return "CRITICAL";
            case AlertLevel::NODATA:
                return "NO_DATA";
            default:
                return "UNKNOWN";
        }
    }

    AlertLevel AlertLevelFromString(const std::string &level) {
        if (level == "OK") return AlertLevel::OK;
        if (level == "WARNING") return AlertLevel::WARNING;
        if (level == "CRITICAL") return AlertLevel::CRITICAL;
        if (level == "NO_DATA") return AlertLevel::NODATA;
        return AlertLevel::UNKNOWN;
    }

    std::string AlertSeverity(const AlertLevel level) {
        switch (level) {
            case AlertLevel::OK:
                return "ok";
            case AlertLevel::WARNING:
                return "warning";
            case AlertLevel::CRITICAL:
                return "critical";
            case AlertLevel::NODATA:
                return "nodata";
            default:
                return "unknown";
        }
    }

    // ── AlertRule ────────────────────────────────────────────────────────────

    bool AlertRule::matches(const std::map<std::string, std::string> &seriesLabels) const {
        for (const auto &[label, value]: labels) {
            const auto found = seriesLabels.find(label);
            if (found == seriesLabels.end() || found->second != value) return false;
        }
        return true;
    }

    bsoncxx::document::value AlertRule::toDocument() const {

        bsoncxx::builder::basic::document document{};
        document.append(kvp("accountId", accountId),
                        kvp("namespace", nameSpace),
                        kvp("ruleId", ruleId),
                        kvp("name", name),
                        kvp("labels", labelDocument(labels)),
                        kvp("resolution", ResolutionToString(resolution)),
                        kvp("statistic", AlertStatisticToString(statistic)),
                        kvp("comparison", AlertComparisonToString(comparison)));
        // Left out rather than written as null: a rule with only a critical line has no warning
        // line, which is a different thing from one at zero.
        if (warning) document.append(kvp("warning", *warning));
        if (critical) document.append(kvp("critical", *critical));
        document.append(kvp("forPeriods", static_cast<std::int64_t>(forPeriods)),
                        kvp("clearPeriods", static_cast<std::int64_t>(clearPeriods)),
                        kvp("noDataAfterPeriods", static_cast<std::int64_t>(noDataAfterPeriods)),
                        kvp("enabled", enabled),
                        kvp("description", description),
                        kvp("created", toDate(created)),
                        kvp("modified", toDate(modified)));
        return document.extract();
    }

    AlertRule AlertRule::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        AlertRule rule;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "_id") rule.oid = oidOf(field);
            else if (key == "accountId") rule.accountId = text(field);
            else if (key == "namespace") rule.nameSpace = text(field);
            else if (key == "ruleId") rule.ruleId = text(field);
            else if (key == "name") rule.name = text(field);
            else if (key == "labels") rule.labels = labelMap(field);
            else if (key == "resolution") rule.resolution = ResolutionFromString(text(field));
            else if (key == "statistic") rule.statistic = AlertStatisticFromString(text(field));
            else if (key == "comparison") rule.comparison = AlertComparisonFromString(text(field));
            else if (key == "warning") rule.warning = number(field);
            else if (key == "critical") rule.critical = number(field);
            else if (key == "forPeriods") rule.forPeriods = integer(field);
            else if (key == "clearPeriods") rule.clearPeriods = integer(field);
            else if (key == "noDataAfterPeriods") rule.noDataAfterPeriods = integer(field);
            else if (key == "enabled" && field.type() == bsoncxx::type::k_bool) rule.enabled = field.get_bool().value;
            else if (key == "description") rule.description = text(field);
            else if (key == "created") rule.created = fromDate(field);
            else if (key == "modified") rule.modified = fromDate(field);
        }
        return rule;
    }

    // ── AlertState ───────────────────────────────────────────────────────────

    bsoncxx::document::value AlertState::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                kvp("accountId", accountId),
                kvp("ruleId", ruleId),
                kvp("labelKey", labelKey),
                kvp("labels", labelDocument(labels)),
                kvp("state", AlertLevelToString(level)),
                kvp("since", toDate(since)),
                kvp("lastValue", lastValue),
                kvp("lastSeen", toDate(lastSeen)),
                kvp("breachStreak", static_cast<std::int64_t>(breachStreak)),
                kvp("criticalStreak", static_cast<std::int64_t>(criticalStreak)),
                kvp("okStreak", static_cast<std::int64_t>(okStreak)),
                kvp("belowCriticalStreak", static_cast<std::int64_t>(belowCriticalStreak)));
    }

    AlertState AlertState::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        AlertState state;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "_id") state.oid = oidOf(field);
            else if (key == "accountId") state.accountId = text(field);
            else if (key == "ruleId") state.ruleId = text(field);
            else if (key == "labelKey") state.labelKey = text(field);
            else if (key == "labels") state.labels = labelMap(field);
            else if (key == "state") state.level = AlertLevelFromString(text(field));
            else if (key == "since") state.since = fromDate(field);
            else if (key == "lastValue") state.lastValue = number(field).value_or(0);
            else if (key == "lastSeen") state.lastSeen = fromDate(field);
            else if (key == "breachStreak") state.breachStreak = integer(field);
            else if (key == "criticalStreak") state.criticalStreak = integer(field);
            else if (key == "okStreak") state.okStreak = integer(field);
            else if (key == "belowCriticalStreak") state.belowCriticalStreak = integer(field);
        }
        return state;
    }

    // ── AlertTransition ──────────────────────────────────────────────────────

    bsoncxx::document::value AlertTransition::toDocument() const {

        bsoncxx::builder::basic::document document{};
        document.append(kvp("accountId", accountId),
                        kvp("namespace", nameSpace),
                        kvp("ruleId", ruleId),
                        kvp("name", name),
                        kvp("labels", labelDocument(labels)),
                        kvp("labelKey", labelKey),
                        kvp("previous", AlertLevelToString(previous)),
                        kvp("state", AlertLevelToString(level)),
                        kvp("value", value));
        if (threshold) document.append(kvp("threshold", *threshold));
        document.append(kvp("statistic", AlertStatisticToString(statistic)),
                        kvp("reason", reason),
                        kvp("timestamp", toDate(timestamp)),
                        kvp("expiresAt", toDate(expiresAt)));
        return document.extract();
    }

    AlertTransition AlertTransition::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        AlertTransition transition;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "_id") transition.oid = oidOf(field);
            else if (key == "accountId") transition.accountId = text(field);
            else if (key == "namespace") transition.nameSpace = text(field);
            else if (key == "ruleId") transition.ruleId = text(field);
            else if (key == "name") transition.name = text(field);
            else if (key == "labels") transition.labels = labelMap(field);
            else if (key == "labelKey") transition.labelKey = text(field);
            else if (key == "previous") transition.previous = AlertLevelFromString(text(field));
            else if (key == "state") transition.level = AlertLevelFromString(text(field));
            else if (key == "value") transition.value = number(field).value_or(0);
            else if (key == "threshold") transition.threshold = number(field);
            else if (key == "statistic") transition.statistic = AlertStatisticFromString(text(field));
            else if (key == "reason") transition.reason = text(field);
            else if (key == "timestamp") transition.timestamp = fromDate(field);
            else if (key == "expiresAt") transition.expiresAt = fromDate(field);
        }
        return transition;
    }

}// namespace Euclid::Database::Entity::Monitoring
