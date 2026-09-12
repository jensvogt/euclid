// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>

// Euclid includes
#include <euclid/core/Permissions.h>
#include <euclid/database/Authorization.h>

namespace Euclid::Database {

    namespace {

        constexpr auto kEverything = "*";

        bool coversNamespace(const Entity::EAM::Grant &grant, const std::string &nameSpace) {
            if (std::ranges::contains(grant.namespaces, kEverything)) return true;
            return std::ranges::contains(grant.namespaces, nameSpace);
        }

        bool coversResource(const Entity::EAM::Grant &grant, const std::string &resourceErn) {
            // An action that names no resource is not narrowed by one. The grant's patterns still
            // apply to the actions that do.
            if (resourceErn.empty()) return true;
            return std::ranges::any_of(grant.resources, [&](const auto &pattern) {
                return Authorization::ResourceMatches(pattern, resourceErn);
            });
        }

    }// namespace

    bool Authorization::ResourceMatches(const std::string &pattern, const std::string &resourceErn) {

        if (pattern == kEverything) return true;
        if (pattern == resourceErn) return true;

        // A trailing "*" and nothing else - "order-*" covers "order-events". A pattern that is only
        // "*" was handled above, so the prefix here is never empty and a grant cannot widen itself
        // to everything by accident.
        if (!pattern.ends_with('*')) return false;
        const auto prefix = std::string_view(pattern).substr(0, pattern.size() - 1);
        return resourceErn.starts_with(prefix);
    }

    AuthorizationResult Authorization::Allows(const AuthorizationRequest &request,
                                              const std::vector<Entity::EAM::Grant> &grants,
                                              const RoleLookup &roles) {

        const auto required = Core::Permissions::Of(request.target, request.action);

        // Before looking at a single grant: an action that is not in the vocabulary cannot be
        // granted by anything. That is what keeps emm and emd unreachable - no permission of theirs
        // exists to be required - and what makes a mistyped action fail closed.
        if (!Core::Permissions::Exists(required)) {
            return {.allowed = false,
                    .reason = "'" + required + "' is not a permission any role can hold"};
        }

        long consideredGrants = 0;

        for (const auto &grant: grants) {

            if (grant.accountId != request.accountId) continue;
            if (!coversNamespace(grant, request.nameSpace)) continue;
            if (!coversResource(grant, request.resourceErn)) continue;

            ++consideredGrants;

            const auto permissions = roles(grant.accountId, grant.role);
            if (!permissions.has_value()) continue;

            for (const auto &granted: *permissions) {
                if (Core::Permissions::Matches(granted, required)) {
                    return {.allowed = true,
                            .reason = "granted by role '" + grant.role + "' to " + grant.principal,
                            .role = grant.role};
                }
            }
        }

        // Two different failures, said differently: nothing applies here at all, or something
        // applies and does not cover this action. The first is usually a missing grant, the second
        // usually the wrong role - and an operator reading a 403 wants to know which.
        if (consideredGrants == 0) {
            return {.allowed = false,
                    .reason = "no grant applies to this caller in account '" + request.accountId +
                              "', namespace '" + request.nameSpace + "'" +
                              (request.resourceErn.empty() ? "" : ", resource '" + request.resourceErn + "'")};
        }

        return {.allowed = false,
                .reason = "no role granted here holds '" + required + "'"};
    }

}// namespace Euclid::Database
