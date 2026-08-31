//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <mutex>
#include <vector>

// Euclid includes
#include <euclid/database/repository/ets/IEtsRepository.h>

namespace Euclid::Database {

    /**
     * @brief Transfer server in-memory database.
     *
     * @par
     * Note that a memory-backed repository cannot carry a transfer server definition across
     * process boundaries, and the design depends on exactly that: ETS writes the definition,
     * euclid-mgr reads it to decide what to run, and the spawned process reads it again to
     * configure itself. Running transfer servers therefore needs the MongoDB repository; this
     * one exists for tests and for a single-process installation.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEtsRepository final : public IEtsRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryEtsRepository &instance() {
            static MemoryEtsRepository transferDatabase;
            return transferDatabase;
        }

        /**
         * @brief Create or update a transfer server
         *
         * @param server transfer server
         * @return stored transfer server
         */
        Entity::ETS::TransferServer upsertServer(Entity::ETS::TransferServer &server) override {
            std::lock_guard lock(_mutex);

            const auto now = std::chrono::system_clock::now();
            if (server.created.time_since_epoch().count() == 0) {
                server.created = now;
            }
            server.modified = now;

            const auto existing = std::ranges::find_if(_store, [&](const auto &row) { return row.serverId == server.serverId; });
            if (existing != _store.end()) {
                server.created = existing->created;
                *existing = server;
            } else {
                _store.push_back(server);
            }
            return server;
        }

        /**
         * @brief Find a transfer server by ID
         *
         * @param serverId server ID
         * @return transfer server, if it exists
         */
        std::optional<Entity::ETS::TransferServer> findServerByServerId(const std::string &serverId) const override {
            std::lock_guard lock(_mutex);
            const auto it = std::ranges::find_if(_store, [&](const auto &row) { return row.serverId == serverId; });
            return it != _store.end() ? std::optional(*it) : std::nullopt;
        }

        /**
         * @brief Find a transfer server by ERN
         *
         * @param ern Euclid resource name
         * @return transfer server, if it exists
         */
        std::optional<Entity::ETS::TransferServer> findServerByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            const auto it = std::ranges::find_if(_store, [&](const auto &row) { return row.ern == ern; });
            return it != _store.end() ? std::optional(*it) : std::nullopt;
        }

        /**
         * @brief Whether a transfer server exists
         *
         * @param serverId server ID
         * @return true if it exists
         */
        bool serverExists(const std::string &serverId) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::any_of(_store, [&](const auto &row) { return row.serverId == serverId; });
        }

        /**
         * @brief List transfer servers
         *
         * @param prefix server ID prefix filter
         * @return matching transfer servers
         */
        std::vector<Entity::ETS::TransferServer> listServers(const std::string &prefix) const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::ETS::TransferServer> result;
            for (const auto &row: _store) {
                if (!prefix.empty() && !row.serverId.starts_with(prefix)) continue;
                result.push_back(row);
            }
            std::ranges::sort(result, [](const auto &a, const auto &b) { return a.serverId < b.serverId; });
            return result;
        }

        /**
         * @brief Count transfer servers
         *
         * @return number of transfer servers
         */
        long countServers() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_store.size());
        }

        /**
         * @brief Delete a transfer server
         *
         * @param serverId server ID
         */
        void deleteServer(const std::string &serverId) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_store, [&](const auto &row) { return row.serverId == serverId; });
        }

        /**
         * @brief Remove all transfer servers
         */
        void clear() override {
            std::lock_guard lock(_mutex);
            _store.clear();
        }

    private:

        /**
         * @brief Transfer server mutex
         */
        mutable std::mutex _mutex;

        /**
         * @brief memory database vector.
         */
        std::vector<Entity::ETS::TransferServer> _store;
    };

}// namespace Euclid::Database
