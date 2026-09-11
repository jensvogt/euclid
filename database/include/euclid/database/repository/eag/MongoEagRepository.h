//
// Created by vogje01 on 9/5/26.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/eag/IEagRepository.h>

namespace Euclid::Database {

    /**
     * @brief API gateway route table, in MongoDB.
     *
     * @par
     * The route table is written by EAG (through the CLI or the UI) and read by the gateway
     * process on a timer, so it has to cross a process boundary: whatever holds it has to be
     * shared between processes, whether that is MongoDB or the store EMD keeps.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEagRepository final : public IEagRepository {

    public:

        explicit MongoEagRepository();

        static MongoEagRepository &instance() {
            static MongoEagRepository routeDatabase;
            return routeDatabase;
        }

        std::optional<Entity::EAG::Route> upsertRoute(Entity::EAG::Route &route) override;

        [[nodiscard]]
        std::optional<Entity::EAG::Route> findRouteByRouteId(const std::string &accountId, const std::string &nameSpace,
                                                             const std::string &routeId) const override;

        [[nodiscard]]
        std::optional<Entity::EAG::Route> findRouteByErn(const std::string &ern) const override;

        [[nodiscard]]
        std::vector<Entity::EAG::Route> listRoutes(const std::string &accountId, const std::string &nameSpace,
                                                   const std::string &prefix) const override;

        /**
         * @brief Every route in the installation - for the gateway and the path-claim check only
         *
         * @param prefix path prefix filter
         * @return matching routes
         */
        [[nodiscard]]
        std::vector<Entity::EAG::Route> listAllRoutes(const std::string &prefix) const override;

        [[nodiscard]]
        bool routeExists(const std::string &accountId, const std::string &nameSpace,
                         const std::string &routeId) const override;

        void deleteRoute(const std::string &accountId, const std::string &nameSpace,
                         const std::string &routeId) override;

        [[nodiscard]]
        long countRoutes(const std::string &accountId, const std::string &nameSpace) const override;

        void clear() override;

    private:

        /**
         * @brief The filter that picks exactly one route.
         */
        [[nodiscard]]
        static bsoncxx::document::value routeFilter(const std::string &accountId, const std::string &nameSpace,
                                                    const std::string &routeId);

        /**
         * @brief Routes matching a path prefix, within whatever scope is given.
         *
         * @param prefix path prefix filter; empty matches all.
         * @param scope extra equality fields to filter on, empty for the whole installation.
         */
        [[nodiscard]]
        static std::vector<Entity::EAG::Route> findRoutes(const std::string &prefix, const bsoncxx::document::view &scope);

        /**
         * @brief Collection name
         */
        static constexpr auto COLLECTION = "eag_route";

        /**
         * @brief Creates the indexes the route table needs, if they are not already there.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database
