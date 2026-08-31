#include <TransferAuthenticator.h>

// C++ includes
#include <algorithm>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/PasswordUtils.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::Transfer {

    namespace {

        // Long enough that a large upload cannot outlive the token mid-transfer, short enough
        // that a token leaked out of a process is not useful for long.
        constexpr auto kTokenTtl = std::chrono::hours(12);

    }// namespace

    std::optional<TransferIdentity> TransferAuthenticator::Authenticate(const std::string &userName, const std::string &password) const {

        if (userName.empty() || password.empty()) {
            return std::nullopt;
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto user = repo->findUserByUserId(userName);
        if (!user.has_value()) {
            log_warning << "Transfer login failed, no such EAM user: " << userName;
            return std::nullopt;
        }

        if (!Core::PasswordUtils::Verify(password, user->password)) {
            log_warning << "Transfer login failed, bad password for user: " << userName;
            return std::nullopt;
        }

        if (!isPermitted(user->userId)) {
            // Deliberately distinguished from a bad password in the log but not to the client,
            // which only ever sees a generic failure.
            log_warning << "Transfer login denied, user not permitted on server '" << _server.serverId << "': " << userName;
            return std::nullopt;
        }

        return TransferIdentity{
                .userId = user->userId,
                .accountId = user->accountId,
                .token = Core::JwtUtils::CreateToken(user->userId, Core::HttpActionServer::JwtSecret(), kTokenTtl)};
    }

    bool TransferAuthenticator::isPermitted(const std::string &userId) const {

        if (std::ranges::contains(_server.userIds, userId)) {
            return true;
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        for (const auto &groupName: _server.userGroups) {
            const auto group = repo->findUserGroupByName(groupName);
            if (!group.has_value()) {
                log_warning << "Transfer server '" << _server.serverId << "' references unknown user group: " << groupName;
                continue;
            }
            if (std::ranges::contains(group->userIds, userId)) {
                return true;
            }
        }

        return false;
    }

}// namespace Euclid::Transfer
