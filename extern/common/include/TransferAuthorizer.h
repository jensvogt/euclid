// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <TransferAuthenticator.h>
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::Transfer {

    /**
     * @brief What a transfer client is allowed to do, and why.
     */
    struct TransferDecision {

        bool allowed{false};

        /**
         * @brief One line saying what decided it - logged, never sent to the client.
         *
         * @par
         * An FTP reply of "550 Permission denied" that also said which role was missing would be
         * telling an unauthenticated-adjacent party about the installation's access model. The
         * operator gets the reason in the log; the client gets the refusal.
         */
        std::string reason;
    };

    /**
     * @brief Decides whether a logged-in transfer client may run one command.
     *
     * @par
     * Logging in only ever answered "may this user use this server at all" - the server definition's
     * userIds and userGroups. That is one bit, and a supplier who may drop a file off is thereby
     * also able to delete what is already there. This answers the other question, per command,
     * from the same roles and grants that authorize everything else in euclid: the FTP verb or
     * SFTP packet is turned into an `ets:` permission and put to Database::Authorization::Allows().
     *
     * @par Why not ESM's own checks
     * Every call a session makes to ESM already carries the client's own bearer token, so the
     * bucket's grants do apply. But they apply to the bucket as a whole - there is no ESM
     * permission that means "may upload but not delete", because ESM's delete-object is what both
     * a delete and an overwrite go through. The distinction a transfer server needs is between
     * commands, so it is drawn where the commands are.
     *
     * @par Scope
     * The grant is matched against the transfer server's account, its namespace, and its ERN as
     * the resource - so a grant can be narrowed to one server, and a user with an account-wide
     * grant has it on every server in the account. It is deliberately not matched against the
     * path: a path is confined to the user's home prefix already, and a per-path access model
     * would be a second one to reason about beside the home directory.
     *
     * @author jensvogt47\@gmail.com
     */
    class TransferAuthorizer {

    public:

        /**
         * @brief Constructs an authorizer for one transfer server definition.
         *
         * @param server the definition, whose accountId, nameSpace and ern scope the grants that
         * count.
         */
        explicit TransferAuthorizer(Database::Entity::ETS::TransferServer server) : _server(std::move(server)) {}

        /**
         * @brief Whether this client may run a command, and why.
         *
         * @par
         * Deny-by-default, like every other authorization decision in euclid: a user with no grant
         * that reaches this server is refused every command rather than allowed the harmless ones.
         * Grant the built-in `transfer` role to the users or groups the server already lists - see
         * docs/role-concept.md.
         *
         * @par
         * Installation administrators are allowed everything without holding a grant, the same
         * short-circuit the HTTP gate takes.
         *
         * @param identity the client that logged in.
         * @param action the `ets:` action, without the module prefix - e.g. "put-file".
         * @return the verdict and the reason for it.
         */
        [[nodiscard]]
        TransferDecision Allows(const TransferIdentity &identity, const std::string &action) const;

    private:

        Database::Entity::ETS::TransferServer _server;
    };

}// namespace Euclid::Transfer
