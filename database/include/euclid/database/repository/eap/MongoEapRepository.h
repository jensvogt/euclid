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
         * @brief Find an application by ID, within the account and namespace that owns it
         *
         * @param accountId account the application belongs to
         * @param nameSpace namespace within accountId
         * @param applicationId application ID
         * @return application, if it exists
         */
        [[nodiscard]]
        std::optional<Entity::EAP::Application> findApplicationByApplicationId(const std::string &accountId, const std::string &nameSpace,
                                                                               const std::string &applicationId) const override;

        /**
         * @brief Find an application by ERN
         *
         * @param ern Euclid resource name
         * @return application, if it exists
         */
        [[nodiscard]]
        std::optional<Entity::EAP::Application> findApplicationByErn(const std::string &ern) const override;

        /**
         * @brief Find the application running under a name, wherever it is defined
         *
         * @param runtimeName the name to look for
         * @return application, if one is running under it
         */
        [[nodiscard]]
        std::optional<Entity::EAP::Application> findApplicationByRuntimeName(const std::string &runtimeName) const override;

        /**
         * @brief Whether an application exists in that account and namespace
         *
         * @param accountId account the application would belong to
         * @param nameSpace namespace within accountId
         * @param applicationId application ID
         * @return true if it exists there
         */
        [[nodiscard]]
        bool applicationExists(const std::string &accountId, const std::string &nameSpace,
                               const std::string &applicationId) const override;

        /**
         * @brief List one namespace's applications
         *
         * @param accountId account whose applications to list
         * @param nameSpace namespace within accountId
         * @param prefix application ID prefix filter
         * @return matching applications
         */
        [[nodiscard]]
        std::vector<Entity::EAP::Application> listApplications(const std::string &accountId, const std::string &nameSpace,
                                                               const std::string &prefix) const override;

        /**
         * @brief Every application in the installation - for the manager only
         *
         * @param prefix application ID prefix filter
         * @return matching applications
         */
        [[nodiscard]]
        std::vector<Entity::EAP::Application> listAllApplications(const std::string &prefix) const override;

        /**
         * @brief Count one namespace's applications
         *
         * @return number of applications
         */
        [[nodiscard]]
        long countApplications(const std::string &accountId, const std::string &nameSpace) const override;

        /**
         * @brief Delete an application
         *
         * @param accountId account the application belongs to
         * @param nameSpace namespace within accountId
         * @param applicationId application ID
         */
        void deleteApplication(const std::string &accountId, const std::string &nameSpace,
                               const std::string &applicationId) override;

        /**
         * @brief Sets the level an application's own output is logged at, and nothing else - in
         * particular not the modification date, which the manager reads as a definition change.
         *
         * @param accountId account the application belongs to
         * @param nameSpace namespace within accountId
         * @param applicationId application to change
         * @param logLevel level name, or empty to leave the level to the configuration
         * @return true if an application of that name was changed
         */
        bool setApplicationLogLevel(const std::string &accountId, const std::string &nameSpace,
                                    const std::string &applicationId, const std::string &logLevel) override;

    private:

        /**
         * @brief The filter that picks exactly one application.
         */
        [[nodiscard]]
        static bsoncxx::document::value applicationFilter(const std::string &accountId, const std::string &nameSpace,
                                                          const std::string &applicationId);

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
