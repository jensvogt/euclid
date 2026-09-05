//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eag/Route.h>

namespace Euclid::EAG {

    /**
     * @brief The routes the gateway is currently serving, and which of them a path belongs to.
     *
     * @par
     * Held in memory and refreshed from the database on a timer rather than read per request: a
     * route table changes when somebody configures it, which is rarely, and reading it on the way
     * through would put a database round trip in front of every proxied call. Refreshing rather
     * than being told means a route added through the CLI or the UI reaches the gateway on its own
     * - the same reconcile-from-the-database arrangement the manager uses for transfer servers and
     * applications, and for the same reason: no message can be missed and a restart recovers the
     * whole picture.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class RouteTable {

    public:

        /**
         * @brief Re-reads every active route.
         *
         * @par
         * A failed read leaves the previous table in place. The alternative - serving nothing
         * because the database blinked - would turn a momentary outage there into an outage of
         * every application behind the gateway.
         *
         * @return true if the table was replaced.
         */
        bool refresh();

        /**
         * @brief The route a path belongs to, or nothing if no route claims it.
         *
         * @par
         * Longest prefix wins. A route for "/resource" carries everything beneath it, and
         * a more specific "/resource/export" can be carved out later without either being
         * reordered: the more specific one is simply longer. Two routes claiming the exact same
         * path would be a tie decided by nothing in particular, which is why create-route and
         * update-route refuse to make one.
         *
         * @param path request target, without its query string.
         * @return the matching route.
         */
        [[nodiscard]]
        std::optional<Database::Entity::EAG::Route> match(const std::string &path) const;

        /**
         * @brief The distinct applications the current routes point at.
         *
         * @par
         * Which applications the gateway needs backends for is a property of the route table, so it
         * is answered from the table already in memory rather than by reading the routes a second
         * time. It also keeps the two in step: the backends refreshed are exactly those the routes
         * being served name.
         */
        [[nodiscard]]
        std::vector<std::string> applicationIds() const;

        /**
         * @brief How many routes are currently served.
         */
        [[nodiscard]]
        std::size_t size() const;

    private:

        mutable std::mutex _mutex;

        /**
         * @brief Active routes, longest path first, so the first match is the most specific one.
         */
        std::vector<Database::Entity::EAG::Route> _routes;
    };

}// namespace Euclid::EAG