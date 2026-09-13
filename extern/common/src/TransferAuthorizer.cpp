// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <TransferAuthorizer.h>

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/BuiltinRoles.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Permissions.h>
#include <euclid/database/Authorization.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::Transfer {

    TransferDecision TransferAuthorizer::Allows(const TransferIdentity &identity, const std::string &action) const {

        // Cached on the same terms as the HTTP gate's lookups, and for the same reason: a session
        // runs many commands, and re-reading the user document for each of them would make a
        // directory listing cost a database round trip per entry the client asks about. What can
        // be stale is the user document, for up to the configured auth cache TTL.
        const auto user = Database::UsersByUserId().get(identity.userId, [](const std::string &id) {
            return Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(id);
        });

        // The user existed at login - this is a deleted account mid-session, which is a refusal
        // rather than the "left to the handler" the HTTP gate answers: there is no handler behind
        // this to make its own decision.
        if (!user.has_value()) {
            return {.allowed = false, .reason = "no EAM user '" + identity.userId + "' any more"};
        }

        if (Database::IsCachedEamAdmin(user->userId)) {
            return {.allowed = true, .reason = "member of the administrator user group, which is allowed everything and holds no grants"};
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto result = Database::Authorization::Allows(
                {.target = "ets",
                 .action = action,
                 .accountId = _server.accountId,
                 .nameSpace = _server.nameSpace,
                 .resourceErn = _server.ern},
                repo->findGrantsByPrincipals(Database::PrincipalsOf(*user)),
                [&repo](const std::string &accountId, const std::string &role) -> std::optional<std::vector<std::string>> {
                    if (const auto stored = repo->findRoleByName(accountId, role)) return stored->permissions;
                    if (Core::BuiltinRoles::Exists(role)) return Core::BuiltinRoles::PermissionsOf(role);
                    return std::nullopt;
                });

        return {.allowed = result.allowed, .reason = result.reason};
    }

}// namespace Euclid::Transfer
