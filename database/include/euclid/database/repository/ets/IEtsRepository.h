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
        virtual std::optional<Entity::ETS::TransferServer> findServerByServerId(const std::string &accountId, const std::string &nameSpace,
                                                                                const std::string &serverId) const = 0;

        /**
         * @brief Finds the transfer server running under a name, wherever it is defined.
         *
         * @par
         * The one lookup that spans the installation, because the name it takes does too: a
         * process pool, a unix socket and the --transfer-server argument a spawned server
         * identifies itself by are not partitioned by account or namespace. Used to check a newly
         * issued runtime name is free, and by the spawned process to read its own definition back.
         * Answers for servers from before the field existed as well, which run under their bare id.
         *
         * @param runtimeName the name to look for - see Entity::ETS::RuntimeName().
         * @return the server running under it, or std::nullopt if none is.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ETS::TransferServer> findServerByRuntimeName(const std::string &runtimeName) const = 0;

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
        virtual bool serverExists(const std::string &accountId, const std::string &nameSpace,
                                  const std::string &serverId) const = 0;

        /**
         * @brief Lists transfer servers, ordered by server ID.
         *
         * @param prefix filter by server ID prefix; empty matches all.
         * @return matching servers.
         */
        [[nodiscard]]
        virtual std::vector<Entity::ETS::TransferServer> listServers(const std::string &accountId, const std::string &nameSpace,
                                                                     const std::string &prefix) const = 0;

        /**
         * @brief Every transfer server in the installation, whatever account or namespace defines it.
         *
         * @par
         * For the manager, which runs them all, and for the checks that are about a host rather
         * than about a caller - two servers may not share a TCP port, whoever owns them. Nothing
         * serving one caller should use this.
         *
         * @param prefix only servers whose ID starts with this are returned; empty matches all.
         * @return matching servers, sorted by ID.
         */
        [[nodiscard]]
        virtual std::vector<Entity::ETS::TransferServer> listAllServers(const std::string &prefix) const = 0;

        /**
         * @brief Counts transfer servers.
         *
         * @return number of stored servers.
         */
        [[nodiscard]]
        virtual long countServers(const std::string &accountId, const std::string &nameSpace) const = 0;

        /**
         * @brief Deletes a transfer server.
         *
         * @param serverId server ID.
         */
        virtual void deleteServer(const std::string &accountId, const std::string &nameSpace,
                                  const std::string &serverId) = 0;

        /**
         * @brief Removes every transfer server.
         */
        virtual void clear() = 0;
    };

}// namespace Euclid::Database
