// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <map>
#include <ranges>

// Euclid includes
#include <euclid/core/BuiltinRoles.h>
#include <euclid/core/Permissions.h>

namespace Euclid::Core {

    namespace {

        // The action half of "<module>:<action>". Every entry of Permissions::All() has a colon,
        // which PermissionVocabularyTest holds true.
        std::string_view actionOf(const std::string_view permission) {
            const auto colon = permission.find(':');
            return colon == std::string_view::npos ? permission : permission.substr(colon + 1);
        }

        std::string_view moduleOf(const std::string_view permission) {
            const auto colon = permission.find(':');
            return colon == std::string_view::npos ? permission : permission.substr(0, colon);
        }

        // Every permission the vocabulary holds that satisfies a rule, in vocabulary order - which
        // is sorted, so the result is too.
        std::vector<std::string> select(const auto &rule) {
            std::vector<std::string> selected;
            for (const auto &permission: Permissions::All()) {
                if (rule(permission)) selected.push_back(permission);
            }
            return selected;
        }

        // Reads rather than changes anything. Prefix-matched on the action, because euclid names
        // its actions consistently enough for that to be the honest rule rather than a list that
        // has to be maintained: list-queues, get-object, describe-table, count-objects.
        //
        // `count-` is here because euclid names counting both ways: the cached figures are
        // get-object-count and get-message-count, which the get- prefix already covers, and the
        // one action that counts for real is count-objects. A reader that could list a bucket's
        // objects but not be told how many there are would be a strange thing to have built.
        bool isRead(const std::string_view permission) {
            const auto action = actionOf(permission);
            return action.starts_with("list-") || action.starts_with("get-")
                   || action.starts_with("describe-") || action.starts_with("count-");
        }

        // Removes something that does not come back. Deliberately not "everything that writes":
        // an operator has to be able to create a queue and send to it.
        bool isDestructive(const std::string_view permission) {
            const auto action = actionOf(permission);
            return action.starts_with("delete-") || action.starts_with("purge-");
        }

        const std::vector<std::string> &empty() {
            static const std::vector<std::string> kEmpty;
            return kEmpty;
        }

        // The two roles that are a genuine short list rather than a rule. Every entry is checked
        // against the vocabulary by BuiltinRoleTest, so a renamed action breaks the build rather
        // than quietly emptying a role.
        const std::vector<std::string> &publisherPermissions() {
            static const std::vector<std::string> kPermissions = [] {
                std::vector<std::string> permissions{
                        "ens:get-topic-ern",
                        "ens:publish-message",
                        "eqs:get-queue-ern",
                        "eqs:send-message",
                };
                std::ranges::sort(permissions);
                return permissions;
            }();
            return kPermissions;
        }

        const std::vector<std::string> &consumerPermissions() {
            static const std::vector<std::string> kPermissions = [] {
                std::vector<std::string> permissions{
                        "ens:subscribe",
                        "ens:unsubscribe",
                        "eqs:delete-message",
                        "eqs:get-queue-ern",
                        "eqs:receive-messages",
                        // Both spellings: EQS dispatches the same command under two names, and a
                        // consumer that could only reach one of them would work or not depending on
                        // which its client happened to send.
                        "eqs:set-message-visibility",
                        "eqs:set-visibility",
                };
                std::ranges::sort(permissions);
                return permissions;
            }();
            return kPermissions;
        }

        // What an FTP or SFTP client can do, which is not a rule the vocabulary can express: the
        // transfer permissions are the ets: entries the transfer servers check, and the rest of
        // ets: is server administration that no transfer client should hold.
        //
        // The esm: half is not decoration. A transfer server keeps nothing of its own - a listing
        // is a listing of bucket keys, an upload becomes an object - and every one of those calls
        // is made with the client's own token, so ESM's gate applies to it. A role holding only
        // the ets: half would pass the FTP check and be refused one layer down, which is a role
        // that does not do what its name says.
        const std::vector<std::string> &transferPermissions() {
            static const std::vector<std::string> kPermissions = [] {
                std::vector<std::string> permissions{
                        "ets:create-directory",
                        "ets:delete-directory",
                        "ets:delete-file",
                        "ets:get-file",
                        "ets:list-directory",
                        "ets:put-file",
                        "ets:rename-file",
                        // Exactly what TransferStorage calls, and nothing else: no create-bucket,
                        // no delete-bucket, no subscriptions. A transfer client works inside a
                        // bucket somebody else made for it.
                        "esm:delete-object",
                        "esm:get-object",
                        "esm:list-objects",
                        "esm:put-object",
                };
                std::ranges::sort(permissions);
                return permissions;
            }();
            return kPermissions;
        }

        std::vector<std::string> merged(const std::vector<std::string> &left, const std::vector<std::string> &right,
                                        const std::vector<std::string> &extra) {
            std::vector<std::string> all;
            all.insert(all.end(), left.begin(), left.end());
            all.insert(all.end(), right.begin(), right.end());
            all.insert(all.end(), extra.begin(), extra.end());
            std::ranges::sort(all);
            const auto duplicates = std::ranges::unique(all);
            all.erase(duplicates.begin(), duplicates.end());
            return all;
        }

        const std::map<std::string, std::vector<std::string>> &roles() {

            static const std::map<std::string, std::vector<std::string>> kRoles = [] {
                std::map<std::string, std::vector<std::string>> built;

                built[std::string(BuiltinRoles::AccountAdministrator)] = {std::string(Permissions::Everything)};

                // Everything that is not destructive and is not access management. An operator runs
                // the installation; deciding who may use it is a different job.
                built[std::string(BuiltinRoles::Operator)] = select([](const std::string_view permission) {
                    return !isDestructive(permission) && moduleOf(permission) != "eam";
                });

                built[std::string(BuiltinRoles::Reader)] = select(isRead);

                built[std::string(BuiltinRoles::Publisher)] = publisherPermissions();
                built[std::string(BuiltinRoles::Consumer)] = consumerPermissions();

                // Bound with an explicit resource list, always - which is what makes this narrow
                // despite covering three modules.
                //
                // Publishing and consuming is not enough on its own: an application does not
                // receive from a topic or a bucket, it receives from a queue of its own that it
                // subscribes to one. So it has to be able to make that queue, subscribe it, find
                // it again on the next start, and take it down - including the ones a previous
                // run left behind when it was killed rather than stopped. That is what
                // euclid-spring's listener container does at startup and shutdown, and an
                // application that cannot do it does not start at all.
                built[std::string(BuiltinRoles::Application)] =
                        merged(publisherPermissions(), consumerPermissions(),
                               {"esm:get-object",
                                "esm:put-object",
                                // Its own delivery queue: created on startup, listed to find the
                                // orphans of runs that did not shut down, deleted on the way out.
                                "eqs:create-queue",
                                "eqs:delete-queue",
                                "eqs:list-queues",
                                // And the subscriptions that feed it. list-subscriptions is what
                                // makes a restart idempotent - a listener checks whether it is
                                // already subscribed rather than subscribing twice.
                                "ens:list-subscriptions",
                                "esm:subscribe",
                                "esm:unsubscribe",
                                "esm:list-subscriptions",
                                // Saying how loaded it is, which is the only way the autoscaler
                                // learns anything about an application: it serves no gateway
                                // request, so there is no traffic to observe. Without this the
                                // report is refused, the manager sees nothing, and the pool never
                                // grows however much work is waiting - which is what happened when
                                // the action was added and this was not.
                                "eap:report-load"});

                built[std::string(BuiltinRoles::Transfer)] = transferPermissions();

                return built;
            }();
            return kRoles;
        }

    }// namespace

    const std::vector<std::string> &BuiltinRoles::Names() {
        static const std::vector<std::string> kNames = [] {
            std::vector<std::string> names;
            for (const auto &name: roles() | std::views::keys) names.push_back(name);
            return names;// std::map iterates sorted
        }();
        return kNames;
    }

    bool BuiltinRoles::Exists(const std::string_view name) {
        return roles().contains(std::string(name));
    }

    const std::vector<std::string> &BuiltinRoles::PermissionsOf(const std::string_view name) {
        const auto found = roles().find(std::string(name));
        return found == roles().end() ? empty() : found->second;
    }

    std::string BuiltinRoles::DescriptionOf(const std::string_view name) {
        if (name == AccountAdministrator) return "Every action, within one account";
        if (name == Operator) return "Every action except deleting, purging and access management";
        if (name == Reader) return "Every action that only reads";
        if (name == Publisher) return "Publish to a topic and send to a queue";
        if (name == Consumer) return "Receive from a queue and manage topic subscriptions";
        if (name == Application) return "What a euclid-deployed application is given: publish, consume, read and write objects, and own its delivery queue";
        if (name == Transfer) return "Everything an FTP or SFTP client can do: list, download, upload, rename, create and remove directories, including the bucket objects those become";
        return {};
    }

}// namespace Euclid::Core
