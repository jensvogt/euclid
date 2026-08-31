//
// Created by vogje01 on 8/31/26.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/ets/IEtsRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief Transfer server MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEtsRepository final : public IEtsRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEtsRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEtsRepository &instance() {
            static MongoEtsRepository transferDatabase;
            return transferDatabase;
        }

        /**
         * @brief Create or update a transfer server
         *
         * @param server transfer server
         * @return stored transfer server
         */
        Entity::ETS::TransferServer upsertServer(Entity::ETS::TransferServer &server) override;

        /**
         * @brief Find a transfer server by ID
         *
         * @param serverId server ID
         * @return transfer server, if it exists
         */
        [[nodiscard]]
        std::optional<Entity::ETS::TransferServer> findServerByServerId(const std::string &serverId) const override;

        /**
         * @brief Find a transfer server by ERN
         *
         * @param ern Euclid resource name
         * @return transfer server, if it exists
         */
        [[nodiscard]]
        std::optional<Entity::ETS::TransferServer> findServerByErn(const std::string &ern) const override;

        /**
         * @brief Whether a transfer server exists
         *
         * @param serverId server ID
         * @return true if it exists
         */
        [[nodiscard]]
        bool serverExists(const std::string &serverId) const override;

        /**
         * @brief List transfer servers
         *
         * @param prefix server ID prefix filter
         * @return matching transfer servers
         */
        [[nodiscard]]
        std::vector<Entity::ETS::TransferServer> listServers(const std::string &prefix) const override;

        /**
         * @brief Count transfer servers
         *
         * @return number of transfer servers
         */
        [[nodiscard]]
        long countServers() const override;

        /**
         * @brief Delete a transfer server
         *
         * @param serverId server ID
         */
        void deleteServer(const std::string &serverId) override;

        /**
         * @brief Remove all transfer servers
         */
        void clear() override;

    private:

        /**
         * @brief Collection name
         */
        static constexpr auto COLLECTION = "ets_server";

        /**
         * @brief Creates the indexes required for efficient transfer server queries, if they do not already exist.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database
