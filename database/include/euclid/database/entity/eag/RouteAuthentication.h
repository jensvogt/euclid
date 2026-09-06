//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>

namespace Euclid::Database::Entity::EAG {

    /**
     * @brief What a route requires of a caller before it is proxied.
     *
     * @par
     * A property of the resource rather than of the gateway, because the two kinds of resource
     * sit behind the same listener: a public read a browser makes without credentials, and an
     * operation only a euclid principal may perform. Deciding it per route is what lets one
     * application serve both.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class RouteAuthentication {

        /**
         * @brief Proxied as it arrives. Whatever the application requires, it enforces itself.
         */
        NONE,

        /**
         * @brief A euclid credential is required and verified before anything is forwarded -
         * whatever the euclid gateway accepts, which today is a bearer token, an RFC 9421 message
         * signature or SigV4.
         */
        EUCLID,

        /**
         * @brief HTTP Basic authentication against a euclid user's password.
         *
         * @par
         * For the callers a euclid credential does not suit: a browser, which will prompt for a
         * username and password when it is answered with WWW-Authenticate, and a script with
         * nothing but curl. The password is checked against the same EAM user store everything
         * else uses, and a technical principal - one with loginEnabled false - is refused here as
         * it is at login.
         *
         * @par
         * Only the gateway accepts this, never a euclid module: verifying a password is PBKDF2 at
         * 100,000 iterations, which is right for something that happens once at login and quite
         * wrong for something on every request. The gateway keeps a short-lived record of what it
         * has already verified so a stream of requests costs one hash rather than thousands.
         */
        BASIC,

        UNKNOWN
    };

    static std::map<RouteAuthentication, std::string> RouteAuthenticationNames{
            {RouteAuthentication::NONE, "NONE"},
            {RouteAuthentication::EUCLID, "EUCLID"},
            {RouteAuthentication::BASIC, "BASIC"},
            {RouteAuthentication::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string RouteAuthenticationToString(const RouteAuthentication &authentication) {
        return RouteAuthenticationNames[authentication];
    }

    /**
     * @brief Reads a value, or nothing if it names no known kind.
     *
     * @par
     * Case-insensitive: "euclid" typed on a command line and "EUCLID" as it is stored are the same
     * thing, and a route that silently ends up unprotected because of the difference is the one
     * mistake here that cannot be allowed to be quiet.
     *
     * @par
     * Returning nothing rather than a default is the whole point: whoever asked has to decide what
     * an unrecognised value means, and the two callers want opposite things. Reading a stored
     * value treats it as NONE, because a route must not go offline over a byte nobody can parse;
     * accepting one from a request refuses it, because a caller who asked for EUCLID and got NONE
     * has been handed a public route they believe is protected.
     */
    [[maybe_unused]]
    static std::optional<RouteAuthentication> RouteAuthenticationFromString(const std::string &authentication) {
        auto upper = authentication;
        std::ranges::transform(upper, upper.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });

        const auto it = std::ranges::find_if(RouteAuthenticationNames, [&upper](const auto &pair) { return pair.second == upper; });
        if (it == RouteAuthenticationNames.end() || it->first == RouteAuthentication::UNKNOWN) return std::nullopt;
        return it->first;
    }

}// namespace Euclid::Database::Entity::EAG
