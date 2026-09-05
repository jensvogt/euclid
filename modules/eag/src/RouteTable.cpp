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

            order(routes);

            std::lock_guard lock(_mutex);
            _routes = std::move(routes);
            return true;

        } catch (const std::exception &e) {
            log_error << "Could not refresh the route table, keeping the previous one, error: " << e.what();
        }
        return false;
    }

    void RouteTable::order(std::vector<Database::Entity::EAG::Route> &routes) {

        // Sorted once, by descending path length, so matching is a scan that stops at the first
        // hit rather than a search for the best one.
        std::ranges::sort(routes, [](const auto &left, const auto &right) {
            return left.path.size() > right.path.size();
        });
    }

    RouteTable::Match RouteTable::match(const std::string &path, const std::string &method) const {
        std::lock_guard lock(_mutex);
        return matchIn(_routes, path, method);
    }

    RouteTable::Match RouteTable::matchIn(const std::vector<Database::Entity::EAG::Route> &routes,
                                          const std::string &path, const std::string &method) {

        Match result;

        for (const auto &route: routes) {
            if (route.path.empty() || !path.starts_with(route.path)) continue;

            // A prefix has to end on a segment boundary. Without this "/resource" would
            // also claim "/resource-intern", which is a different resource that happens to
            // start with the same letters.
            const auto end = route.path.size();
            if (path.size() != end && route.path.back() != '/' && path[end] != '/' && path[end] != '?') continue;

            // A route that names no method answers for every one of them, as it always did.
            if (route.methods.empty() || std::ranges::find(route.methods, method) != route.methods.end()) {
                result.route = route;
                return result;
            }

            // The path is this route's, but the method is not. Remember what it would have taken
            // and keep looking - another route may share the path and want exactly this method.
            result.allowed.insert(route.methods.begin(), route.methods.end());
        }
        return result;
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