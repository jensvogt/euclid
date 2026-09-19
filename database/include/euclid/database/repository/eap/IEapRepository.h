// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
     * @author jensvogt47\@gmail.com
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
         * @brief Finds an application by its ID, within the account and namespace that owns it.
         *
         * @par
         * An applicationId is unique only within (accountId, nameSpace) - the same three fields
         * the unique index is built on - so all three are needed to name one application.
         * Throughout this interface an empty nameSpace means the account's unscoped applications,
         * not "any namespace".
         *
         * @param accountId account the application belongs to.
         * @param nameSpace namespace within accountId the application belongs to.
         * @param applicationId application ID.
         * @return the application, or std::nullopt if no application has that ID there.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAP::Application> findApplicationByApplicationId(const std::string &accountId, const std::string &nameSpace,
                                                                                       const std::string &applicationId) const = 0;

        /**
         * @brief Finds an application by its ERN.
         *
         * @param ern Euclid resource name.
         * @return the application, or std::nullopt if no application has that ERN.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAP::Application> findApplicationByErn(const std::string &ern) const = 0;

        /**
         * @brief Finds the application running under a name, wherever it is defined.
         *
         * @par
         * The one lookup that spans the installation, because the name it takes does too: a
         * process pool, a data directory and a unix socket are not partitioned by account or
         * namespace. Used to check a newly issued runtime name is free, and answers for
         * applications from before the field existed as well, which run under their bare id.
         *
         * @param runtimeName the name to look for - see Entity::EAP::RuntimeName().
         * @return the application running under it, or std::nullopt if none is.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAP::Application> findApplicationByRuntimeName(const std::string &runtimeName) const = 0;

        /**
         * @brief Whether an application with this ID exists in that account and namespace.
         *
         * @param accountId account the application would belong to.
         * @param nameSpace namespace within accountId.
         * @param applicationId application ID.
         * @return true if it exists there.
         */
        [[nodiscard]]
        virtual bool applicationExists(const std::string &accountId, const std::string &nameSpace,
                                       const std::string &applicationId) const = 0;

        /**
         * @brief Lists one namespace's applications, optionally filtered by an ID prefix.
         *
         * @param accountId account whose applications to list.
         * @param nameSpace namespace within accountId whose applications to list.
         * @param prefix only applications whose ID starts with this are returned; empty matches all.
         * @return matching applications, sorted by ID.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAP::Application> listApplications(const std::string &accountId, const std::string &nameSpace,
                                                                       const std::string &prefix) const = 0;

        /**
         * @brief Every application in the installation, whatever account or namespace it belongs to.
         *
         * @par
         * For the manager, which runs them all: the processes on a host are not partitioned by
         * account or namespace, and a reconciler that only saw one namespace's applications would
         * tear down every other namespace's as undefined. Nothing serving a request should use
         * this - a caller sees their own namespace, through listApplications() above.
         *
         * @param prefix only applications whose ID starts with this are returned; empty matches all.
         * @return matching applications, sorted by ID.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAP::Application> listAllApplications(const std::string &prefix) const = 0;

        /**
         * @brief Number of applications in one namespace of an account.
         *
         * @return the count.
         */
        [[nodiscard]]
        virtual long countApplications(const std::string &accountId, const std::string &nameSpace) const = 0;

        /**
         * @brief Removes an application by its ID.
         *
         * @param accountId account the application belongs to.
         * @param nameSpace namespace within accountId.
         * @param applicationId application ID.
         */
        virtual void deleteApplication(const std::string &accountId, const std::string &nameSpace,
                                       const std::string &applicationId) = 0;

        /**
         * @brief Sets the level an application's own output is logged at.
         *
         * @par
         * Its own method rather than a field set through upsertApplication(), because that stamps
         * the modification date - and the manager restarts an application whose definition changed
         * since it started it. Turning a log down is not a change of definition, and restarting a
         * running application to do it would be a worse cure than the noise.
         *
         * @param accountId account the application belongs to
         * @param nameSpace namespace within accountId
         * @param applicationId application to change
         * @param logLevel level name, or empty to leave the level to the configuration
         * @return true if an application of that name was changed
         */
        virtual bool setApplicationLogLevel(const std::string &accountId, const std::string &nameSpace,
                                            const std::string &applicationId, const std::string &logLevel) = 0;

        /**
         * @brief Stamps an application's modification date, so the manager starts its instances
         * again.
         *
         * @par
         * The whole of what a restart is. The manager records the revision each instance was
         * started with - the modification date as it stood then - and on every reconcile stops a
         * pool whose application has been modified since, then starts it back up from the current
         * definition. That is how a redeploy takes effect, and it is the same mechanism here with
         * nothing to pick up: the definition is untouched, so what comes back is what was running.
         *
         * @par
         * Only that one field, and written by the server rather than sent by it, for the reason
         * setApplicationLogLevel() gives: upsertApplication() writes the whole document from the
         * caller's copy, so restarting through it would also revert anything changed since that
         * copy was read.
         *
         * @param accountId account the application belongs to
         * @param nameSpace namespace within accountId
         * @param applicationId application to restart
         * @return true if an application of that name was stamped
         */
        virtual bool touchApplication(const std::string &accountId, const std::string &nameSpace,
                                      const std::string &applicationId) = 0;
    };

}// namespace Euclid::Database
