//
// Created by vogje01 on 9/1/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eap/Application.h>

namespace Euclid::Database {

    /**
     * @brief Interface for application repository operations.
     *
     * @par
     * Read by two processes with different needs: the EAP module (the only writer) and
     * euclid-mgr's reconciler, which lists everything to decide which application processes
     * should be running. Unlike a transfer server, an application never reads its own definition
     * back - it is handed what it needs through its environment, so it needs no database access
     * of its own and can be written in any language.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class IEapRepository {

    public:

        virtual ~IEapRepository() = default;

        /**
         * @brief Creates or updates an application, keyed on applicationId.
         *
         * @param application application to store.
         * @return the stored application.
         */
        virtual Entity::EAP::Application upsertApplication(Entity::EAP::Application &application) = 0;

        /**
         * @brief Finds an application by its ID.
         *
         * @param applicationId application ID.
         * @return the application, or std::nullopt if no application has that ID.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAP::Application> findApplicationByApplicationId(const std::string &applicationId) const = 0;

        /**
         * @brief Finds an application by its ERN.
         *
         * @param ern Euclid resource name.
         * @return the application, or std::nullopt if no application has that ERN.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAP::Application> findApplicationByErn(const std::string &ern) const = 0;

        /**
         * @brief Whether an application with this ID exists.
         *
         * @param applicationId application ID.
         * @return true if it exists.
         */
        [[nodiscard]]
        virtual bool applicationExists(const std::string &applicationId) const = 0;

        /**
         * @brief Lists applications, optionally filtered by an ID prefix.
         *
         * @param prefix only applications whose ID starts with this are returned; empty matches all.
         * @return matching applications, sorted by ID.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAP::Application> listApplications(const std::string &prefix) const = 0;

        /**
         * @brief Total number of applications.
         *
         * @return the count.
         */
        [[nodiscard]]
        virtual long countApplications() const = 0;

        /**
         * @brief Removes an application by its ID.
         *
         * @param applicationId application ID.
         */
        virtual void deleteApplication(const std::string &applicationId) = 0;
    };

}// namespace Euclid::Database
