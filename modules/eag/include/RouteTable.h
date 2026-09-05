//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
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
         * @brief What matching a request found.
         */
        struct Match {

            /**
             * @brief The route that claimed it, if one did.
             */
            std::optional<Database::Entity::EAG::Route> route;

            /**
             * @brief The methods some route does answer for on this path, when none answered for
             * the one asked. Empty unless the path matched and the method did not.
             *
             * @par
             * Kept so the gateway can answer 405 with an Allow header rather than 404. The two
             * mean quite different things to whoever is holding the URL: one says the resource is
             * not there, the other says it is and they asked the wrong way round.
             */
            std::set<std::string> allowed;
        };

        /**
         * @brief The route a path and method belong to, or nothing if no route claims them.
         *
         * @par
         * Longest prefix wins. A route for "/resource" carries everything beneath it, and
         * a more specific "/resource/export" can be carved out later without either being
         * reordered: the more specific one is simply longer. Two routes claiming the exact same
         * path would be a tie decided by nothing in particular, which is why create-route and
         * update-route refuse to make one - unless their methods differ, see below.
         *
         * @par
         * A route that names no method answers for all of them. Where a path matches but the
         * method does not, the scan continues - two routes may share a path and split it by
         * method - and only if none of them wants it are the methods that were on offer reported
         * back in Match::allowed.
         *
         * @param path request target, without its query string.
         * @param method request method, upper case.
         * @return what was found.
         */
        [[nodiscard]]
        Match match(const std::string &path, const std::string &method) const;

        /**
         * @brief Orders routes so that the first one that matches is the most specific.
         *
         * @par
         * Separate from refresh() so that the ordering and the matching below can be exercised
         * together without a database: what these two do between them *is* the routing, and it is
         * the part with rules worth stating and therefore worth testing.
         */
        static void order(std::vector<Database::Entity::EAG::Route> &routes);

        /**
         * @brief Matches against an already-ordered route list. See match().
         */
        [[nodiscard]]
        static Match matchIn(const std::vector<Database::Entity::EAG::Route> &routes,
                             const std::string &path, const std::string &method);

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