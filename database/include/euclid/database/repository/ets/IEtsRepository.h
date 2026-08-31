//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::Database {

    /**
     * @brief Interface for transfer server repository operations.
     *
     * @par
     * Read by three processes with quite different needs: the ETS module (the only writer),
     * euclid-mgr's reconciler (which lists everything to decide what to run), and each spawned
     * euclid-ftp/euclid-sftp process (which reads back only its own definition).
     */
    class IEtsRepository {

    public:

        virtual ~IEtsRepository() = default;

        /**
         * @brief Creates or updates a transfer server, keyed on serverId.
         *
         * @param server server to store.
         * @return the stored server.
         */
        virtual Entity::ETS::TransferServer upsertServer(Entity::ETS::TransferServer &server) = 0;

        /**
         * @brief Finds a transfer server by its ID.
         *
         * @param serverId server ID.
         * @return the server, or std::nullopt if no server has that ID.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ETS::TransferServer> findServerByServerId(const std::string &serverId) const = 0;

        /**
         * @brief Finds a transfer server by its ERN.
         *
         * @param ern Euclid resource name.
         * @return the server, or std::nullopt if no server has that ERN.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ETS::TransferServer> findServerByErn(const std::string &ern) const = 0;

        /**
         * @brief Whether a server with this ID exists.
         *
         * @param serverId server ID.
         * @return true if it exists.
         */
        [[nodiscard]]
        virtual bool serverExists(const std::string &serverId) const = 0;

        /**
         * @brief Lists transfer servers, ordered by server ID.
         *
         * @param prefix filter by server ID prefix; empty matches all.
         * @return matching servers.
         */
        [[nodiscard]]
        virtual std::vector<Entity::ETS::TransferServer> listServers(const std::string &prefix) const = 0;

        /**
         * @brief Counts transfer servers.
         *
         * @return number of stored servers.
         */
        [[nodiscard]]
        virtual long countServers() const = 0;

        /**
         * @brief Deletes a transfer server.
         *
         * @param serverId server ID.
         */
        virtual void deleteServer(const std::string &serverId) = 0;

        /**
         * @brief Removes every transfer server.
         */
        virtual void clear() = 0;
    };

}// namespace Euclid::Database
