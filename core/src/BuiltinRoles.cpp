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
        // has to be maintained: list-queues, get-object, describe-table.
        bool isRead(const std::string_view permission) {
            const auto action = actionOf(permission);
            return action.starts_with("list-") || action.starts_with("get-") || action.starts_with("describe-");
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
                built[std::string(BuiltinRoles::Application)] =
                        merged(publisherPermissions(), consumerPermissions(), {"esm:get-object", "esm:put-object"});

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
        if (name == Application) return "What a euclid-deployed application is given: publish, consume, read and write objects";
        if (name == Transfer) return "Everything an FTP or SFTP client can do: list, download, upload, rename, create and remove directories";
        return {};
    }

}// namespace Euclid::Core
