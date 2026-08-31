#pragma once

// C++ includes
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::Transfer {

    /**
     * @brief A client that has successfully logged in to a transfer server.
     */
    struct TransferIdentity {

        /**
         * @brief EAM user ID this client authenticated as.
         */
        std::string userId;

        /**
         * @brief Account the user belongs to.
         */
        std::string accountId;

        /**
         * @brief Bearer token minted for this user, used for every subsequent call this session
         * makes to ESM.
         *
         * @par
         * Minting a token for the real user, rather than calling ESM as some service account,
         * is what keeps the storage module's own authorization meaningful: an FTP client ends
         * up with exactly the bucket access its EAM user already had, and ESM's per-account and
         * per-namespace grant checks apply unchanged.
         */
        std::string token;
    };

    /**
     * @brief Authenticates transfer server clients against EAM.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class TransferAuthenticator {

    public:

        /**
         * @brief Constructs an authenticator for one transfer server definition.
         *
         * @param server the definition, whose userIds/userGroups decide who may log in.
         */
        explicit TransferAuthenticator(Database::Entity::ETS::TransferServer server) : _server(std::move(server)) {}

        /**
         * @brief Verifies a username and password and checks the user may use this server.
         *
         * @par
         * Three things have to hold: the EAM user exists and is not disabled, the password
         * verifies against the stored hash, and the user is permitted on this server - either
         * listed directly in userIds or a member of one of userGroups.
         *
         * @param userName EAM user ID supplied by the client.
         * @param password plaintext password supplied by the client.
         * @return the authenticated identity, or std::nullopt if any of the three fails.
         */
        [[nodiscard]]
        std::optional<TransferIdentity> Authenticate(const std::string &userName, const std::string &password) const;

    private:

        /**
         * @brief Whether this user is permitted on this server, by direct listing or group
         * membership.
         */
        [[nodiscard]]
        bool isPermitted(const std::string &userId) const;

        Database::Entity::ETS::TransferServer _server;
    };

}// namespace Euclid::Transfer
