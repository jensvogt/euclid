//
// Created by vogje01 on 9/5/26.
//

// C++ includes
#include <algorithm>
#include <set>

// Euclid includes
#include <RouteTable.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAG {

    bool RouteTable::refresh() {

        try {
            auto routes = Database::RepositoryFactory::instance().eagRepository()->listRoutes("");

            // Inactive routes are dropped here rather than filtered on every request: a route
            // somebody has taken out of service is not one the gateway has to keep considering.
            std::erase_if(routes, [](const auto &route) { return !route.active; });

            // Sorted once, by descending path length, so match() is a scan that stops at the first
            // hit rather than a search for the best one.
            std::ranges::sort(routes, [](const auto &left, const auto &right) {
                return left.path.size() > right.path.size();
            });

            std::lock_guard lock(_mutex);
            _routes = std::move(routes);
            return true;

        } catch (const std::exception &e) {
            log_error << "Could not refresh the route table, keeping the previous one, error: " << e.what();
        }
        return false;
    }

    std::optional<Database::Entity::EAG::Route> RouteTable::match(const std::string &path) const {

        std::lock_guard lock(_mutex);
        for (const auto &route: _routes) {
            if (route.path.empty() || !path.starts_with(route.path)) continue;

            // A prefix has to end on a segment boundary. Without this "/resource" would
            // also claim "/resource-intern", which is a different resource that happens to
            // start with the same letters.
            if (path.size() == route.path.size() || route.path.back() == '/' || path[route.path.size()] == '/' || path[route.path.size()] == '?') {
                return route;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> RouteTable::applicationIds() const {

        std::set<std::string> distinct;
        {
            std::lock_guard lock(_mutex);
            for (const auto &route: _routes) distinct.insert(route.applicationId);
        }
        return {distinct.begin(), distinct.end()};
    }

    std::size_t RouteTable::size() const {
        std::lock_guard lock(_mutex);
        return _routes.size();
    }

}// namespace Euclid::EAG