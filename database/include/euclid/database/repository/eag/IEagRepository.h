//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eag/Route.h>

namespace Euclid::Database {

    /**
     * @brief Storage for the API gateway's route table.
     *
     * @par
     * Small and read constantly: the gateway re-reads every route on a timer so a change made
     * through the CLI reaches it without anything being restarted, in the same way the manager
     * picks up transfer server and application definitions.
     */
    class IEagRepository {

    public:

        virtual ~IEagRepository() = default;

        /**
         * @brief Inserts a new route or updates the existing one with the same routeId.
         *
         * @param route the route to store.
         * @return the persisted route.
         */
        virtual Entity::EAG::Route upsertRoute(Entity::EAG::Route &route) = 0;

        /**
         * @brief Finds a route by the name it is managed under.
         *
         * @param routeId route name.
         * @return the route, or std::nullopt if there is none.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAG::Route> findRouteByRouteId(const std::string &routeId) const = 0;

        /**
         * @brief Finds a route by its ERN.
         *
         * @param ern Euclid resource name.
         * @return the route, or std::nullopt if there is none.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAG::Route> findRouteByErn(const std::string &ern) const = 0;

        /**
         * @brief Every route, whether active or not.
         *
         * @par
         * Inactive routes are returned too: the gateway decides what to serve, and a listing that
         * silently omitted them would make a route somebody had disabled look deleted.
         *
         * @param prefix only routes whose routeId starts with this; empty matches all.
         * @return the routes.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAG::Route> listRoutes(const std::string &prefix) const = 0;

        /**
         * @brief Whether a route of this name exists.
         *
         * @param routeId route name.
         * @return true if it exists.
         */
        [[nodiscard]]
        virtual bool routeExists(const std::string &routeId) const = 0;

        /**
         * @brief Deletes a route by the name it is managed under.
         *
         * @param routeId route name.
         */
        virtual void deleteRoute(const std::string &routeId) = 0;

        /**
         * @brief Total number of routes.
         *
         * @return the count.
         */
        [[nodiscard]]
        virtual long countRoutes() const = 0;

        /**
         * @brief Removes every route.
         */
        virtual void clear() = 0;
    };

}// namespace Euclid::Database
