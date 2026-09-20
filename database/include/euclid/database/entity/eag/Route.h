// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
#include <euclid/database/entity/eag/RouteType.h>

namespace Euclid::Database::Entity::EAG {

    /**
     * @brief Where an upload route puts what it receives, and what it will accept.
     *
     * @par
     * Only meaningful on a route of type UPLOAD, which is why it is a document of its own rather
     * than five more fields on Route: a proxy route carrying an empty bucket and a zero part size
     * would invite the question of what they mean, and the answer is nothing.
     */
    struct UploadSpec {

        /**
         * @brief ERN of the bucket objects are written to.
         */
        std::string bucket;

        /**
         * @brief Prefix every key is confined to, or empty for the whole bucket.
         *
         * @par
         * The home directory of a transfer server, in a bucket: without it anybody who may upload
         * at all may overwrite any key the bucket has. See TransferAuthorizer for why the same
         * argument applies to FTP - ESM's grants are per bucket, and "may add but not replace" is
         * not something they can say.
         */
        std::string keyPrefix;

        /**
         * @brief Largest body this route accepts, in bytes. 0 is no limit beyond the listener's.
         *
         * @par
         * Per route because it is a property of what is published: a route taking ONIX deliveries
         * and one taking profile pictures have nothing to say to each other about size.
         */
        long maxBytes{};

        /**
         * @brief Bytes per part streamed to ESM, and the threshold under which a body is written
         * with one put-object instead of a multipart upload.
         */
        long partSize{};

        /**
         * @brief Content types accepted, or empty for any.
         */
        std::vector<std::string> contentTypes;
    };

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
     * @author jensvogt47\@gmail.com
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
         * @brief What the gateway does with a request this route matches.
         *
         * @par
         * PROXY unless it says otherwise, which is what every route written before uploads existed
         * means and what most routes written after will still mean.
         */
        RouteType type = RouteType::PROXY;

        /**
         * @brief Where an UPLOAD route writes, and what it accepts. Unused by a PROXY route.
         */
        UploadSpec upload;

        /**
         * @brief Application that serves this path.
         *
         * @par
         * The applicationId as EAP knows it, which is also the name of the module pool the
         * manager runs for it - which is how its instances, and therefore its ports, are found.
         */
        std::string applicationId;

        /**
         * @brief Euclid module this route reaches instead of an application, e.g. "eam". Empty for
         * an ordinary route.
         *
         * @par
         * The way in for something outside euclid that needs euclid itself rather than an
         * application it runs - a browser that has to log in before it can call anything, most of
         * all. Without it a front end would talk to the API gateway for the application and to
         * euclid's own gateway for its credentials: two ports, two origins, and CORS between them.
         *
         * @par
         * The request is forwarded to euclid's own gateway rather than to the module, because a
         * module listens on a Unix domain socket and has no address anything outside the host
         * could reach. The gateway is what turns a target and an action into a call on one, and it
         * is already the thing every SDK talks to.
         */
        std::string moduleTarget;

        /**
         * @brief The one action moduleTarget answers for on this route, e.g. "login".
         *
         * @par
         * One action per route, deliberately, rather than reading it from the rest of the path. A
         * route for "/euclid" that passed its remaining segments through as actions would publish
         * every action the module has, including the ones that delete users - and publishing an
         * administrative interface by accident is not a mistake that announces itself. Naming the
         * action means what is exposed is exactly what somebody wrote down.
         */
        std::string moduleAction;

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