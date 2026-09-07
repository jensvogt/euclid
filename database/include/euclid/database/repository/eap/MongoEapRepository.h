//
// Created by vogje01 on 9/1/26.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/eap/IEapRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief Application MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEapRepository final : public IEapRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEapRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEapRepository &instance() {
            static MongoEapRepository applicationDatabase;
            return applicationDatabase;
        }

        /**
         * @brief Create or update an application
         *
         * @param application application
         * @return stored application
         */
        Entity::EAP::Application upsertApplication(Entity::EAP::Application &application) override;

        /**
         * @brief Find an application by ID
         *
         * @param applicationId application ID
         * @return application, if it exists
         */
        [[nodiscard]]
        std::optional<Entity::EAP::Application> findApplicationByApplicationId(const std::string &applicationId) const override;

        /**
         * @brief Find an application by ERN
         *
         * @param ern Euclid resource name
         * @return application, if it exists
         */
        [[nodiscard]]
        std::optional<Entity::EAP::Application> findApplicationByErn(const std::string &ern) const override;

        /**
         * @brief Whether an application exists
         *
         * @param applicationId application ID
         * @return true if it exists
         */
        [[nodiscard]]
        bool applicationExists(const std::string &applicationId) const override;

        /**
         * @brief List applications
         *
         * @param prefix application ID prefix filter
         * @return matching applications
         */
        [[nodiscard]]
        std::vector<Entity::EAP::Application> listApplications(const std::string &prefix) const override;

        /**
         * @brief Count applications
         *
         * @return number of applications
         */
        [[nodiscard]]
        long countApplications() const override;

        /**
         * @brief Delete an application
         *
         * @param applicationId application ID
         */
        void deleteApplication(const std::string &applicationId) override;

        /**
         * @brief Sets the level an application's own output is logged at, and nothing else - in
         * particular not the modification date, which the manager reads as a definition change.
         *
         * @param applicationId application to change
         * @param logLevel level name, or empty to leave the level to the configuration
         * @return true if an application of that name was changed
         */
        bool setApplicationLogLevel(const std::string &applicationId, const std::string &logLevel) override;

    private:

        /**
         * @brief Collection name
         */
        static constexpr auto COLLECTION = "eap_application";

        /**
         * @brief Creates the indexes required for efficient application queries, if they do not already exist.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database
