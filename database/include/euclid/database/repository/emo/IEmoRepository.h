// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/emo/Alert.h>
#include <euclid/database/entity/emo/Dashboard.h>
#include <euclid/database/entity/emo/MonitoringData.h>

namespace Euclid::Database {

    /**
     * @brief Filter for monitoring data queries.
     *
     * Every string filter is optional and an empty one matches all values; an epoch-valued from/to
     * leaves that end of the time range open.
     */
    struct MonitoringQuery {

        /**
         * @brief Filter by metric name; empty matches all names.
         */
        std::string name;

        /**
         * @brief Filter by dimensions; every entry given must match, and a row may carry more.
         *
         * The multi-dimensional form of labelName/labelValue below, which stay for the callers -
         * and the wire format - that only ever narrow by one.
         */
        std::map<std::string, std::string> labels;

        /**
         * @brief Filter by label name; empty matches all label names.
         */
        std::string labelName;

        /**
         * @brief Filter by label value; empty matches all label values.
         */
        std::string labelValue;

        /**
         * @brief Resolution tier to read from.
         */
        Entity::Monitoring::Resolution resolution{Entity::Monitoring::Resolution::RAW};

        /**
         * @brief Start of the time range, inclusive; epoch leaves it open.
         */
        std::chrono::system_clock::time_point from{};

        /**
         * @brief End of the time range, exclusive; epoch leaves it open.
         */
        std::chrono::system_clock::time_point to{};

        /**
         * @brief Maximum number of rows to return; 0 or less means no limit.
         */
        long limit{};
    };

    /**
     * @brief Interface for monitoring data repository operations.
     *
     * @par
     * Provides an abstraction for storing and querying the aggregated monitoring data points
     * written periodically by the monitoring module, across the three resolution tiers described
     * in Entity::Monitoring::Resolution.
     */
    /**
     * @brief Filter for IEmoRepository::listAlertHistory(). Empty fields filter nothing.
     */
    struct AlertHistoryQuery {
        std::string accountId;
        std::string ruleId;
        std::chrono::system_clock::time_point from{};
        std::chrono::system_clock::time_point to{};
        long limit{};
    };

    /**
     * @brief One metric as IEmoRepository::listMetrics() reports it: what it is called, how many
     * series it has and which labels tell them apart.
     */
    struct MetricSummary {
        std::string name;
        Entity::Monitoring::MetricType type{Entity::Monitoring::MetricType::GAUGE};
        long series{};
        std::vector<std::string> labelNames;
        std::chrono::system_clock::time_point lastSeen{};
    };

    class IEmoRepository {

    public:

        virtual ~IEmoRepository() = default;

        /**
         * @brief Writes one aggregated monitoring data point, replacing any existing point for the
         * same series and bucket.
         *
         * @par
         * Upsert rather than insert because both writers are idempotent by design: the flush task
         * writes a bucket-aligned timestamp, and a rollup recomputes whole buckets, including the
         * still-incomplete current one, on every run.
         *
         * @param data the data point to write; identified by name/labelName/labelValue/resolution/timestamp.
         */
        /**
         * @brief What the storage backend reports about its own size.
         *
         * @par
         * Two different sizes, because they answer different questions. dataSize is what the
         * documents come to uncompressed - "how much data do we hold"; storageSize is what they
         * occupy on disk after compression, which on a euclid installation is several times
         * smaller. totalSize (storage plus indexes) is the one that fills a disk.
         */
        struct DatabaseStats {
            long collections{};
            long objects{};
            long dataSize{};
            long storageSize{};
            long indexSize{};
            long totalSize{};
        };

        /**
         * @brief Reads the backend's own size counters, or nothing if it has none.
         *
         * @par
         * Cheap enough to call on a monitoring tick: WiredTiger keeps these as running counters, so
         * answering costs a command round trip rather than a scan of anything. The in-memory
         * backend has nothing equivalent and returns std::nullopt, which is not an error - it is
         * what "there is no database to measure" looks like.
         *
         * @return the backend's sizes, or std::nullopt if it does not keep any.
         */
        [[nodiscard]]
        virtual std::optional<DatabaseStats> databaseStats() const = 0;

        virtual void upsert(const Entity::Monitoring::MonitoringData &data) = 0;

        /**
         * @brief Lists monitoring data points, most recent first.
         *
         * @param query filter to apply.
         * @return matching data points, sorted by timestamp descending.
         */
        [[nodiscard]]
        virtual std::vector<Entity::Monitoring::MonitoringData> list(const MonitoringQuery &query) const = 0;

        /**
         * @brief Returns an average over all labelName/labelValue of one metric.
         *
         * @param query filter to apply; limit is ignored.
         * @return average value, or 0 if nothing matched.
         */
        [[nodiscard]]
        virtual double average(const MonitoringQuery &query) const = 0;

        /**
         * @brief Aggregates one resolution tier into the next coarser one.
         *
         * @par
         * Reads only the tier named by from, so a daily rollup touches the 24 hourly rows of a
         * series rather than its 288 raw ones. Buckets are recomputed rather than appended to, so
         * re-running an overlapping window is safe and repairs any gap left by downtime.
         *
         * @param from tier to read.
         * @param to tier to write; its bucket width decides the grouping.
         * @param windowStart start of the range of source rows to aggregate, inclusive.
         * @param windowEnd end of the range of source rows to aggregate, exclusive.
         * @param retention how long the written buckets should be kept, used to set their expiresAt.
         * @return number of buckets written.
         */
        virtual long rollup(Entity::Monitoring::Resolution from, Entity::Monitoring::Resolution to,
                            std::chrono::system_clock::time_point windowStart, std::chrono::system_clock::time_point windowEnd,
                            std::chrono::seconds retention) = 0;

        /**
         * @brief Deletes every data point whose expiresAt has passed.
         *
         * @param now point in time to evaluate expiry against.
         * @return number of data points deleted.
         */
        virtual long deleteExpired(std::chrono::system_clock::time_point now) = 0;

        /**
         * @brief Every metric with a RAW row since a point in time, by name.
         *
         * @par
         * From the rows rather than from a list of names anybody keeps: a metric exists in EMO
         * because something pushed it, and that is the only record there is.
         *
         * @param since the oldest row to consider
         * @return the metrics, sorted by name
         */
        [[nodiscard]]
        virtual std::vector<MetricSummary> listMetrics(std::chrono::system_clock::time_point since) const = 0;

        // ── Saved dashboards ──────────────────────────────────────────────────
        //
        // Layouts rather than measurements, kept here because a dashboard describes what to read
        // out of this module. They are stored apart from the data points and share none of their
        // behaviour: no resolution, no rollup, and above all no retention - see Entity::Dashboard.

        /**
         * @brief Writes a dashboard, replacing whatever that user had stored under that name.
         *
         * @par
         * Upsert rather than insert because saving is what the client does on every explicit save
         * of a layout somebody is still editing, and an "already exists" on the second save would
         * mean the client had to ask first and then write - two round trips to answer a question
         * the write itself answers.
         *
         * @param dashboard the dashboard to write; identified by userId and name.
         */
        virtual void upsertDashboard(const Entity::Monitoring::Dashboard &dashboard) = 0;

        /**
         * @brief Reads one user's dashboard by name.
         *
         * @param userId owner, as EAM knows them.
         * @param name dashboard name.
         * @return the dashboard, or nothing when that user has none by that name.
         */
        [[nodiscard]]
        virtual std::optional<Entity::Monitoring::Dashboard> getDashboard(const std::string &userId, const std::string &name) const = 0;

        /**
         * @brief Lists one user's dashboards, most recently modified first.
         *
         * @par
         * The layouts come back with them. A list is read to populate a chooser, which needs only
         * the names - but a user has a handful of dashboards, not thousands, and a second round
         * trip per name to open one is worse than sending a few kilobytes nobody reads.
         *
         * @param userId owner, as EAM knows them.
         * @return that user's dashboards; empty when they have none.
         */
        [[nodiscard]]
        virtual std::vector<Entity::Monitoring::Dashboard> listDashboards(const std::string &userId) const = 0;

        /**
         * @brief Removes one user's dashboard by name.
         *
         * @param userId owner, as EAM knows them.
         * @param name dashboard name.
         * @return number of dashboards removed: 1, or 0 when there was no such dashboard.
         */
        virtual long deleteDashboard(const std::string &userId, const std::string &name) = 0;

        // ── Alerting ──────────────────────────────────────────────────────────
        //
        // Rules, the state each matched series is in, and the history of its changes - three
        // collections apart from the data points for the same reason dashboards are: none of them
        // is a measurement, and only the history expires. Writes answer whether they happened,
        // because an alert event is published only once its state is on disk.

        /**
         * @brief Writes a rule, replacing the account's rule of the same ruleId.
         *
         * @param rule rule to write
         * @return true if it was written
         */
        virtual bool upsertAlertRule(const Entity::Monitoring::AlertRule &rule) = 0;

        /**
         * @brief Reads one rule.
         *
         * @param accountId account
         * @param ruleId rule
         * @return the rule, or nothing
         */
        [[nodiscard]]
        virtual std::optional<Entity::Monitoring::AlertRule> getAlertRule(const std::string &accountId, const std::string &ruleId) const = 0;

        /**
         * @brief Lists rules, by ruleId.
         *
         * @param accountId one account's, or every account's when empty - the evaluator judges the
         *                  installation's metrics against all of them
         * @return the rules
         */
        [[nodiscard]]
        virtual std::vector<Entity::Monitoring::AlertRule> listAlertRules(const std::string &accountId) const = 0;

        /**
         * @brief Removes one rule. Its states are the caller's to remove.
         *
         * @return number of rules removed
         */
        virtual long deleteAlertRule(const std::string &accountId, const std::string &ruleId) = 0;

        /**
         * @brief Lists the states of the series rules have matched.
         *
         * @param accountId one account's, or every account's when empty
         * @param ruleId one rule's, or every rule's when empty
         * @return the states
         */
        [[nodiscard]]
        virtual std::vector<Entity::Monitoring::AlertState> listAlertStates(const std::string &accountId, const std::string &ruleId) const = 0;

        /**
         * @brief Writes one series' state, replacing the one for the same rule and series.
         *
         * @return true if it was written
         */
        virtual bool upsertAlertState(const Entity::Monitoring::AlertState &state) = 0;

        /**
         * @brief Removes every state of one rule.
         *
         * @return number removed
         */
        virtual long deleteAlertStates(const std::string &accountId, const std::string &ruleId) = 0;

        /**
         * @brief Records one change of state.
         *
         * @return true if it was written
         */
        virtual bool insertAlertTransition(const Entity::Monitoring::AlertTransition &transition) = 0;

        /**
         * @brief Lists recorded changes of state, newest first.
         *
         * @param query which account, rule and time range, and how many
         * @return the transitions
         */
        [[nodiscard]]
        virtual std::vector<Entity::Monitoring::AlertTransition> listAlertHistory(const AlertHistoryQuery &query) const = 0;
    };

}// namespace Euclid::Database
