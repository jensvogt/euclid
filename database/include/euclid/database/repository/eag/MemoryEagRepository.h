//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <mutex>
#include <ranges>
#include <vector>

// Euclid includes
#include <euclid/database/repository/eag/IEagRepository.h>

namespace Euclid::Database {

    /**
     * @brief API gateway route table, in memory.
     *
     * @par
     * As with transfer servers, a route written here cannot be seen by another process: the CLI
     * writes routes and euclid-eag reads them, and in a memory-backed installation those are the
     * same process only when everything runs in one. Real use needs the MongoDB repository; this
     * one is for tests.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEagRepository final : public IEagRepository {

    public:

        std::optional<Entity::EAG::Route> upsertRoute(Entity::EAG::Route &route) override {
            std::lock_guard lock(_mutex);
            const auto now = std::chrono::system_clock::now();
            if (route.created.time_since_epoch().count() == 0) route.created = now;
            route.modified = now;
            _routes[route.routeId] = route;
            return route;
        }

        [[nodiscard]]
        std::optional<Entity::EAG::Route> findRouteByRouteId(const std::string &routeId) const override {
            std::lock_guard lock(_mutex);
            const auto it = _routes.find(routeId);
            return it != _routes.end() ? std::optional(it->second) : std::nullopt;
        }

        [[nodiscard]]
        std::optional<Entity::EAG::Route> findRouteByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &route: _routes | std::views::values) {
                if (route.ern == ern) return route;
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::vector<Entity::EAG::Route> listRoutes(const std::string &prefix) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::EAG::Route> routes;
            for (const auto &route: _routes | std::views::values) {
                if (prefix.empty() || route.path.starts_with(prefix)) routes.push_back(route);
            }
            return routes;
        }

        [[nodiscard]]
        bool routeExists(const std::string &routeId) const override {
            std::lock_guard lock(_mutex);
            return _routes.contains(routeId);
        }

        void deleteRoute(const std::string &routeId) override {
            std::lock_guard lock(_mutex);
            _routes.erase(routeId);
        }

        [[nodiscard]]
        long countRoutes() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_routes.size());
        }

        void clear() override {
            std::lock_guard lock(_mutex);
            _routes.clear();
        }

    private:

        mutable std::mutex _mutex;
        std::map<std::string, Entity::EAG::Route> _routes;
    };

}// namespace Euclid::Database
