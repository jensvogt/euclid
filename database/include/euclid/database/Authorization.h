// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eam/Grant.h>
#include <euclid/database/entity/eam/Role.h>

namespace Euclid::Database {

    /**
     * @brief What a request is asking to do.
     */
    struct AuthorizationRequest {

        /**
         * @brief Module target, e.g. "ens" - the x-euclid-target header.
         */
        std::string target;

        /**
         * @brief Module action, e.g. "publish-message" - the x-euclid-action header.
         */
        std::string action;

        /**
         * @brief Account the request is acting in - the x-euclid-account-id header.
         */
        std::string accountId;

        /**
         * @brief Namespace the request is scoped to, empty for the account root.
         */
        std::string nameSpace;

        /**
         * @brief The resource being acted on, for the actions that name one.
         *
         * @par
         * Empty when the action names no resource, and a grant's resource patterns are then not
         * consulted. Which actions name one is not decided here - see docs/role-concept.md §4.1.
         */
        std::string resourceErn;
    };

    /**
     * @brief Why a request was allowed or refused.
     *
     * @par
     * The reason is the point. "Can this user do that" is asked far less often than "why can they",
     * and a system that cannot say gets worked around by making everybody an administrator.
     */
    struct AuthorizationResult {

        /**
         * @brief Whether the request may proceed.
         */
        bool allowed{false};

        /**
         * @brief In one line, what decided it - the grant and role that allowed it, or what was
         * missing.
         */
        std::string reason;

        /**
         * @brief The role whose grant allowed the request, when one did.
         */
        std::string role;
    };

    /**
     * @brief Decides whether a caller may do something, from their grants alone.
     *
     * @par
     * The whole model, in one function: the union of the grants held by a principal and by each
     * group they belong to, filtered by account, namespace and resource, matched against the
     * permission the request needs. No ordering, no precedence, no deny rules - which is what makes
     * the answer readable rather than simulated.
     *
     * @par
     * Deliberately takes the grants and roles rather than fetching them: this is the part worth
     * testing exhaustively, and it has no business knowing about repositories, caches or headers.
     * The two short-circuits that live above it - the `administrator` user group and the `system`
     * principal - are the caller's to apply, and are not expressible as grants by design.
     *
     * @par
     * See docs/role-concept.md §4.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Authorization {

    public:

        /**
         * @brief Resolves a role name to what it grants.
         *
         * @par
         * A stored role of the account, or a built-in one. Answers std::nullopt when the name is
         * neither, which refuses the grant that named it rather than treating it as empty - a grant
         * pointing at a role that is gone should be visible, not silently inert.
         */
        using RoleLookup = std::function<std::optional<std::vector<std::string>>(const std::string &accountId, const std::string &role)>;

        /**
         * @brief Whether a request is allowed by these grants.
         *
         * @param request what is being asked.
         * @param grants  every grant that applies to the caller - their own and their groups'.
         * @param roles   resolves a grant's role name to its permissions.
         * @return the decision, and why.
         */
        [[nodiscard]]
        static AuthorizationResult Allows(const AuthorizationRequest &request,
                                          const std::vector<Entity::EAM::Grant> &grants,
                                          const RoleLookup &roles);

        /**
         * @brief Whether an ERN pattern covers a resource.
         *
         * @par
         * Exact, or a trailing "*" - "ern:ens:...:topic:order-*" covers "order-events". "*" on its
         * own covers everything. Nothing else: a pattern language is a thing to get wrong, and
         * these two forms are what a namespace of resources actually looks like.
         *
         * @param pattern     what a grant holds.
         * @param resourceErn what the request names.
         * @return true if the pattern covers it.
         */
        [[nodiscard]]
        static bool ResourceMatches(const std::string &pattern, const std::string &resourceErn);
    };

}// namespace Euclid::Database
