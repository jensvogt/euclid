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
     * process on a timer, so it has to cross a process boundary - which is why real use needs
     * this rather than the in-memory repository.
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
        std::optional<Entity::EAG::Route> findRouteByRouteId(const std::string &routeId) const override;

        [[nodiscard]]
        std::optional<Entity::EAG::Route> findRouteByErn(const std::string &ern) const override;

        [[nodiscard]]
        std::vector<Entity::EAG::Route> listRoutes(const std::string &prefix) const override;

        [[nodiscard]]
        bool routeExists(const std::string &routeId) const override;

        void deleteRoute(const std::string &routeId) override;

        [[nodiscard]]
        long countRoutes() const override;

        void clear() override;

    private:

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
