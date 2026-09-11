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
        std::optional<Entity::ETS::TransferServer> findServerByServerId(const std::string &accountId, const std::string &nameSpace,
                                                                        const std::string &serverId) const override;

        /**
         * @brief Find the server running under a name, wherever it is defined
         *
         * @param runtimeName the name to look for
         * @return server, if one is running under it
         */
        [[nodiscard]]
        std::optional<Entity::ETS::TransferServer> findServerByRuntimeName(const std::string &runtimeName) const override;

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
        bool serverExists(const std::string &accountId, const std::string &nameSpace,
                          const std::string &serverId) const override;

        /**
         * @brief List transfer servers
         *
         * @param prefix server ID prefix filter
         * @return matching transfer servers
         */
        [[nodiscard]]
        std::vector<Entity::ETS::TransferServer> listServers(const std::string &accountId, const std::string &nameSpace,
                                                             const std::string &prefix) const override;

        /**
         * @brief Every transfer server in the installation - for the manager and host-wide checks
         *
         * @param prefix server ID prefix filter
         * @return matching servers
         */
        [[nodiscard]]
        std::vector<Entity::ETS::TransferServer> listAllServers(const std::string &prefix) const override;

        /**
         * @brief Count transfer servers
         *
         * @return number of transfer servers
         */
        [[nodiscard]]
        long countServers(const std::string &accountId, const std::string &nameSpace) const override;

        /**
         * @brief Delete a transfer server
         *
         * @param serverId server ID
         */
        void deleteServer(const std::string &accountId, const std::string &nameSpace,
                          const std::string &serverId) override;

        /**
         * @brief Remove all transfer servers
         */
        void clear() override;

    private:

        /**
         * @brief The filter that picks exactly one transfer server.
         */
        [[nodiscard]]
        static bsoncxx::document::value serverFilter(const std::string &accountId, const std::string &nameSpace,
                                                     const std::string &serverId);

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
