//
// Created by vogje01 on 9/1/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <mutex>
#include <vector>

// Euclid includes
#include <euclid/database/repository/eap/IEapRepository.h>

namespace Euclid::Database {

    /**
     * @brief Application in-memory database.
     *
     * @par
     * A memory-backed repository cannot carry an application definition across process
     * boundaries, and the design depends on exactly that: EAP writes the definition and
     * euclid-mgr reads it to decide what to run. Running applications therefore needs the
     * MongoDB repository; this one exists for tests and for a single-process installation.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEapRepository final : public IEapRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryEapRepository &instance() {
            static MemoryEapRepository applicationDatabase;
            return applicationDatabase;
        }

        /**
         * @brief Create or update an application
         *
         * @param application application
         * @return stored application
         */
        Entity::EAP::Application upsertApplication(Entity::EAP::Application &application) override {
            std::lock_guard lock(_mutex);

            const auto now = std::chrono::system_clock::now();
            if (application.created.time_since_epoch().count() == 0) {
                application.created = now;
            }
            application.modified = now;

            const auto existing = std::ranges::find_if(_store, [&](const auto &row) { return row.applicationId == application.applicationId; });
            if (existing != _store.end()) {
                application.created = existing->created;
                *existing = application;
            } else {
                _store.push_back(application);
            }
            return application;
        }

        /**
         * @brief Find an application by ID
         *
         * @param applicationId application ID
         * @return application, if it exists
         */
        std::optional<Entity::EAP::Application> findApplicationByApplicationId(const std::string &applicationId) const override {
            std::lock_guard lock(_mutex);
            const auto it = std::ranges::find_if(_store, [&](const auto &row) { return row.applicationId == applicationId; });
            if (it == _store.end()) return std::nullopt;
            return *it;
        }

        /**
         * @brief Find an application by ERN
         *
         * @param ern Euclid resource name
         * @return application, if it exists
         */
        std::optional<Entity::EAP::Application> findApplicationByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            const auto it = std::ranges::find_if(_store, [&](const auto &row) { return row.ern == ern; });
            if (it == _store.end()) return std::nullopt;
            return *it;
        }

        /**
         * @brief Whether an application exists
         *
         * @param applicationId application ID
         * @return true if it exists
         */
        bool applicationExists(const std::string &applicationId) const override {
            return findApplicationByApplicationId(applicationId).has_value();
        }

        /**
         * @brief List applications, optionally filtered by ID prefix
         *
         * @param prefix application ID prefix; empty matches all
         * @return matching applications, sorted by ID
         */
        std::vector<Entity::EAP::Application> listApplications(const std::string &prefix) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::EAP::Application> result;
            for (const auto &row: _store) {
                if (prefix.empty() || row.applicationId.starts_with(prefix)) result.push_back(row);
            }
            std::ranges::sort(result, {}, &Entity::EAP::Application::applicationId);
            return result;
        }

        /**
         * @brief Total number of applications
         *
         * @return the count
         */
        long countApplications() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_store.size());
        }

        /**
         * @brief Remove an application by ID
         *
         * @param applicationId application ID
         */
        void deleteApplication(const std::string &applicationId) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_store, [&](const auto &row) { return row.applicationId == applicationId; });
        }

    private:

        mutable std::mutex _mutex;
        std::vector<Entity::EAP::Application> _store;
    };

}// namespace Euclid::Database
