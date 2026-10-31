// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/emo/IEmoRepository.h>

namespace Euclid::Database {

    /**
     * @brief Monitoring data MongoDB database.
     *
     * @author jensvogt47\@gmail.com
     */
    class MongoEmoRepository final : public IEmoRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEmoRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEmoRepository &instance() {
            static MongoEmoRepository monitoringDatabase;
            return monitoringDatabase;
        }

        /**
         * @brief Write one monitoring data point, replacing any existing point for the same bucket
         *
         * @param data monitoring data
         */
        void upsert(const Entity::Monitoring::MonitoringData &data) override;

        /**
         * @brief Return a list of monitoring data
         *
         * @param query filter to apply
         * @return list of monitoring metrics
         */
        [[nodiscard]]
        std::vector<Entity::Monitoring::MonitoringData> list(const MonitoringQuery &query) const override;

        /**
         * @brief Returns an average over all labelName/labelValue
         *
         * @param query filter to apply
         * @return average value
         */
        [[nodiscard]]
        double average(const MonitoringQuery &query) const override;

        /**
         * @brief Aggregate one resolution tier into the next coarser one
         *
         * @param from tier to read
         * @param to tier to write
         * @param windowStart start of the source range, inclusive
         * @param windowEnd end of the source range, exclusive
         * @param retention how long the written buckets should be kept
         * @return number of buckets written
         */
        long rollup(Entity::Monitoring::Resolution from, Entity::Monitoring::Resolution to,
                    std::chrono::system_clock::time_point windowStart, std::chrono::system_clock::time_point windowEnd,
                    std::chrono::seconds retention) override;

        /**
         * @brief Clean up expired monitoring data
         *
         * @param now point in time to evaluate expiry against
         * @return number of data points deleted
         */
        long deleteExpired(std::chrono::system_clock::time_point now) override;

        /**
         * @brief Write a dashboard, replacing that user's previous one of the same name
         *
         * @param dashboard dashboard to write
         */
        void upsertDashboard(const Entity::Monitoring::Dashboard &dashboard) override;

        /**
         * @brief Read one user's dashboard by name
         *
         * @param userId owner
         * @param name dashboard name
         * @return the dashboard, or nothing
         */
        [[nodiscard]]
        std::optional<Entity::Monitoring::Dashboard> getDashboard(const std::string &userId, const std::string &name) const override;

        /**
         * @brief List one user's dashboards, most recently modified first
         *
         * @param userId owner
         * @return that user's dashboards
         */
        [[nodiscard]]
        std::vector<Entity::Monitoring::Dashboard> listDashboards(const std::string &userId) const override;

        /**
         * @brief Remove one user's dashboard by name
         *
         * @param userId owner
         * @param name dashboard name
         * @return number of dashboards removed
         */
        long deleteDashboard(const std::string &userId, const std::string &name) override;

        bool upsertAlertRule(const Entity::Monitoring::AlertRule &rule) override;

        [[nodiscard]]
        std::optional<Entity::Monitoring::AlertRule> getAlertRule(const std::string &accountId, const std::string &ruleId) const override;

        [[nodiscard]]
        std::vector<Entity::Monitoring::AlertRule> listAlertRules(const std::string &accountId) const override;

        long deleteAlertRule(const std::string &accountId, const std::string &ruleId) override;

        [[nodiscard]]
        std::vector<Entity::Monitoring::AlertState> listAlertStates(const std::string &accountId, const std::string &ruleId) const override;

        bool upsertAlertState(const Entity::Monitoring::AlertState &state) override;

        long deleteAlertStates(const std::string &accountId, const std::string &ruleId) override;

        bool insertAlertTransition(const Entity::Monitoring::AlertTransition &transition) override;

        [[nodiscard]]
        std::vector<Entity::Monitoring::AlertTransition> listAlertHistory(const AlertHistoryQuery &query) const override;

    private:

        /**
         * @brief Collection name
         */
        static constexpr auto COLLECTION = "emo_data";

        /**
         * @brief Collection holding saved dashboard layouts.
         *
         * @par
         * Apart from the data points rather than beside them: COLLECTION carries a TTL index on
         * expiresAt, and a dashboard has no expiry to put there. A row without that field is left
         * alone by MongoDB's TTL monitor, so this is not what keeps dashboards alive - but nothing
         * whose job is to prune measurements should ever hold a cursor over documents a person
         * wrote, and one collection for both would mean exactly that.
         */
        static constexpr auto DASHBOARD_COLLECTION = "emo_dashboard";

        /**
         * @brief Alert rules, one per account and ruleId. Kept until deleted.
         */
        static constexpr auto ALERT_RULE_COLLECTION = "emo_alert_rule";

        /**
         * @brief Where each series a rule has matched stands. Kept until its rule is deleted.
         */
        static constexpr auto ALERT_STATE_COLLECTION = "emo_alert_state";

        /**
         * @brief Every change of state, each expiring on its own expiresAt.
         */
        static constexpr auto ALERT_HISTORY_COLLECTION = "emo_alert_history";

        /**
         * @brief Creates the indexes required for efficient monitoring data queries, if they do not already exist.
         */
        static void ensureIndexes();

        [[nodiscard]]
        std::optional<DatabaseStats> databaseStats() const override;
    };

}// namespace Euclid::Database
