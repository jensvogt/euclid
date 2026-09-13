// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>
#include <string_view>
#include <vector>

namespace Euclid::Core {

    /**
     * @brief The roles every installation has without anybody creating them.
     *
     * @par
     * Nobody should have to write out 189 permissions to grant somebody the right to publish. These
     * seven cover what an installation actually asks for, and a role of an account's own is for the
     * cases they do not.
     *
     * @par Why they are not stored
     * Roles are otherwise per account - see docs/role-concept.md §3.2 - and these are the one
     * exception, deliberately. Copying six roles into every account at creation would mean every
     * account's `operator` goes stale the day a module gains an action, and an installation with
     * fifty accounts would need a migration to fix each one. So they are computed here, referenced
     * by name from any account, readable everywhere and writable nowhere.
     *
     * @par Why most of them are computed rather than listed
     * `reader` is "every action that reads" and `operator` is "everything that is not destructive".
     * Those are rules about the vocabulary, so they are applied to Permissions::All() rather than
     * transcribed from it - a module that gains a `list-widgets` action gains it in `reader` on the
     * same build, with nobody to remember. The two that are genuinely a short list -
     * `publisher` and `consumer` - are listed, and BuiltinRoleTest checks every entry exists.
     *
     * @par
     * There is deliberately no `administrator` role. Installation administration is membership of
     * the `administrator` user group, not a binding, because roles are per account and an
     * installation administrator is by definition not - and because EMM has to be reachable by
     * something, and no emm permission exists to grant.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class BuiltinRoles {

    public:

        /**
         * @brief Everything, within the account the binding names.
         *
         * @par
         * "Everything" is Permissions::Everything, which does not reach the unbindable modules -
         * so this is the strongest thing a role can say, and it is still not installation
         * administration.
         */
        static constexpr std::string_view AccountAdministrator = "account-administrator";

        /**
         * @brief Every action except the destructive ones and access management.
         */
        static constexpr std::string_view Operator = "operator";

        /**
         * @brief Every action that only reads.
         */
        static constexpr std::string_view Reader = "reader";

        /**
         * @brief Send to a queue, publish to a topic, and resolve the two by name.
         */
        static constexpr std::string_view Publisher = "publisher";

        /**
         * @brief Take messages off a queue and manage a topic subscription.
         */
        static constexpr std::string_view Consumer = "consumer";

        /**
         * @brief What a euclid-deployed application is given: publish, consume, and read and write
         * objects - always bound with an explicit resource list.
         */
        static constexpr std::string_view Application = "application";

        /**
         * @brief Everything an FTP or SFTP client can do: list, download, upload, rename, and
         * create and remove directories - including the bucket objects those turn into.
         *
         * @par
         * The one built-in role that exists for a migration as much as for a use case. Transfer
         * clients used to be authorized by being listed on the server and nothing else, so
         * checking permissions there would otherwise mean every existing client stops working
         * until somebody writes a role by hand. This is that role, written once.
         *
         * @par Why it spans two modules
         * A transfer server stores nothing of its own. A listing is a listing of bucket keys, an
         * upload becomes an object, and every one of those calls is made with the client's own
         * token - so ESM's gate applies to it just as much as the `ets:` check does. A role
         * holding only the `ets:` half would pass the FTP check and be refused one layer down.
         * The four `esm:` entries are exactly what Transfer::TransferStorage calls and no more.
         *
         * @par
         * The consequence to know about: this reaches those four ESM actions from *any* client,
         * not only through FTP. A principal holding it can put an object with the SDK. That is
         * inherent in granting the ability rather than the protocol, and scoping the grant's
         * resources to the one bucket is what bounds it.
         *
         * @par
         * It is a genuine short list rather than a rule because "everything a transfer client can
         * do" is not a shape the vocabulary has, and the point of granting something narrower
         * (say, read-only) is to grant fewer than these.
         */
        static constexpr std::string_view Transfer = "transfer";

        /**
         * @brief Every built-in role's name, sorted.
         */
        static const std::vector<std::string> &Names();

        /**
         * @brief Whether a name is one of these.
         *
         * @param name role name.
         * @return true if it is built in, and therefore not an account's own.
         */
        [[nodiscard]]
        static bool Exists(std::string_view name);

        /**
         * @brief What a built-in role grants, sorted.
         *
         * @param name role name.
         * @return its permissions, or an empty list when the name is not a built-in one. The list
         * may contain wildcards - see Permissions::Matches().
         */
        static const std::vector<std::string> &PermissionsOf(std::string_view name);

        /**
         * @brief A one-line description, for `list-roles`.
         *
         * @param name role name.
         * @return the description, or empty when the name is not a built-in one.
         */
        [[nodiscard]]
        static std::string DescriptionOf(std::string_view name);
    };

}// namespace Euclid::Core
