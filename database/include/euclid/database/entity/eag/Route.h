//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/eag/RouteAuthentication.h>

namespace Euclid::Database::Entity::EAG {

    /**
     * @brief One resource the API gateway serves: a path, and the application behind it.
     *
     * @par
     * The gateway routes by configuration rather than by convention. A caller asks for
     * "/resource/searchById?id=123" and has no idea which application answers it - that
     * is what a route says, and it is why the application's name never appears in the URL. The
     * same arrangement lets an application be renamed, replaced or split across several routes
     * without anything that calls it having to change.
     *
     * @par
     * The route says nothing about *where* the application is. Its instances come and go as the
     * autoscaler grows and shrinks the pool, and their ports are assigned when they start, so the
     * gateway reads them from the module repository at the moment it needs one rather than
     * recording them here.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Route {

        /**
         * @brief Unique identifier for this document.
         */
        std::string oid;

        /**
         * @brief Name this route is managed under, unique across the installation.
         */
        std::string routeId;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Account this route belongs to
         */
        std::string accountId;

        /**
         * @brief Region this route belongs to
         */
        std::string region;

        /**
         * @brief Namespace this route belongs to
         */
        std::string nameSpace;

        /**
         * @brief Path this route answers for, matched as a prefix.
         *
         * @par
         * A prefix rather than an exact path, because a REST resource is a tree: one route for
         * "/resource" carries every operation beneath it, and nobody has to enumerate
         * them. Where two routes both match, the longer one wins - so a general route can be
         * placed over an application and a more specific one carved out of it later without
         * either being reordered or rewritten.
         */
        std::string path;

        /**
         * @brief Application that serves this path.
         *
         * @par
         * The applicationId as EAP knows it, which is also the name of the module pool the
         * manager runs for it - which is how its instances, and therefore its ports, are found.
         */
        std::string applicationId;

        /**
         * @brief HTTP methods this route answers for, upper case. Empty means all of them.
         *
         * @par
         * Empty rather than a list of every method, so "all" keeps meaning all: a route written
         * before PATCH was in anybody's vocabulary should not quietly refuse it. A route that
         * names methods is making a decision; one that names none is not.
         *
         * @par
         * Two routes may share a path if their methods do not overlap, which is what lets one
         * resource be served by two applications - reads by one, writes by another - without the
         * caller seeing a seam. That is also why the method is part of matching rather than a
         * filter applied afterwards: a GET that a POST-only route would have rejected has to be
         * allowed to fall through to the route that does want it.
         */
        std::vector<std::string> methods;

        /**
         * @brief What a caller must present before a request is forwarded.
         */
        RouteAuthentication authentication = RouteAuthentication::NONE;

        /**
         * @brief Whether the gateway serves this route at all.
         *
         * @par
         * A route can be taken out of service without being deleted, which is what makes it
         * possible to stop exposing something in a hurry and put it back afterwards knowing it
         * returns exactly as it was.
         */
        bool active = true;

        /**
         * @brief Time this route was created.
         */
        std::chrono::system_clock::time_point created = std::chrono::system_clock::now();

        /**
         * @brief Time this route was last modified.
         */
        std::chrono::system_clock::time_point modified = std::chrono::system_clock::now();

        /**
         * @brief Converts this route to a BSON document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Builds a Route from a BSON document.
         *
         * @param doc document to read, or std::nullopt for an empty route.
         */
        [[maybe_unused]]
        static Route fromDocument(const std::optional<bsoncxx::document::view> &doc);
    };

}// namespace Euclid::Database::Entity::EAG