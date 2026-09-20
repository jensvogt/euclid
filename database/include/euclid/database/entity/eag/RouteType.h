// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
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
     * @brief What the gateway does with a request this route matches.
     *
     * @par
     * Until there was something to upload to, every route did the same thing - read a request and
     * forward it - and the only question was where to. That is PROXY, and it is still what almost
     * every route is. UPLOAD is the one that does not forward: the gateway is the endpoint, and
     * what it does with the body is write it into a bucket.
     *
     * @par
     * The distinction is not "which backend" but "who reads the body". A proxied request is
     * buffered whole and handed on; an upload is streamed straight through to ESM in parts and is
     * never held in memory. That is why this is a type on the route rather than another field on
     * the one behaviour - the two cannot share a request path.
     *
     * @author jensvogt47\@gmail.com
     */
    enum class RouteType {

        /**
         * @brief Forward the request to an application or to a euclid module.
         */
        PROXY,

        /**
         * @brief Terminate the request here and write its body into a bucket.
         */
        UPLOAD,

        UNKNOWN
    };

    static std::map<RouteType, std::string> RouteTypeNames{
            {RouteType::PROXY, "PROXY"},
            {RouteType::UPLOAD, "UPLOAD"},
            {RouteType::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string RouteTypeToString(const RouteType &type) {
        return RouteTypeNames[type];
    }

    /**
     * @brief Reads a value, or nothing if it names no known kind.
     *
     * @par
     * Case-insensitive, and returning nothing rather than a default for the same reason
     * RouteAuthenticationFromString does: the two callers want opposite things from an
     * unrecognised value. Reading a stored one treats it as PROXY, which fails safe - a route that
     * should have been an upload endpoint then names no application and no module, finds no
     * backend, and answers 503. Accepting one from a request refuses it, because a caller who
     * asked for UPLOAD and silently got PROXY has configured an endpoint that will never store
     * anything.
     */
    [[maybe_unused]]
    static std::optional<RouteType> RouteTypeFromString(const std::string &type) {
        auto upper = type;
        std::ranges::transform(upper, upper.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });

        const auto it = std::ranges::find_if(RouteTypeNames, [&upper](const auto &pair) { return pair.second == upper; });
        if (it == RouteTypeNames.end() || it->first == RouteType::UNKNOWN) return std::nullopt;
        return it->first;
    }

}// namespace Euclid::Database::Entity::EAG
