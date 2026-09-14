// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <ranges>
#include <utility>

// Euclid includes
#include <euclid/core/Permissions.h>

namespace Euclid::Core {

    namespace {

        // The module of a "<module>:<action>" permission, or the whole string when there is no
        // colon - which Matches() then fails to match, since no entry of All() is colon-less.
        std::string_view moduleOf(const std::string_view permission) {
            const auto colon = permission.find(':');
            return colon == std::string_view::npos ? permission : permission.substr(0, colon);
        }

    }// namespace

    const std::vector<std::string> &Permissions::All() {

        // Sorted as a whole, not merely within each block: the modules are listed alphabetically
        // and their actions alphabetically, and since every entry starts with its module name, that
        // makes the vector sorted - which is what Exists() binary-searches. Grouping it by module
        // as well costs nothing and is how a reader looks for what ENS offers.
        //
        // PermissionVocabularyTest re-derives this from modules/*/src/*Server.cpp and fails with
        // the exact lines to add or remove. Do not edit it to make a build pass without adding the
        // action the module actually dispatches - an entry here that no module answers grants
        // nothing, silently.
        static const std::vector<std::string> kAll = {
                // eag - 6 actions
                "eag:create-route",
                "eag:delete-route",
                "eag:get-metrics",
                "eag:get-route",
                "eag:list-routes",
                "eag:update-route",

                // eam - 37 actions
                "eam:change-namespace",
                "eam:check-permission",
                "eam:create-access-key",
                "eam:create-account",
                "eam:create-namespace",
                "eam:create-role",
                "eam:create-user-group",
                "eam:delete-access-key",
                "eam:delete-account",
                "eam:delete-namespace",
                "eam:delete-role",
                "eam:delete-user",
                "eam:delete-user-group",
                "eam:get-metrics",
                "eam:get-role",
                "eam:grant-role",
                "eam:list-access-keys",
                "eam:list-accounts",
                "eam:list-grants",
                "eam:list-namespaces",
                "eam:list-permissions",
                "eam:list-roles",
                "eam:list-user-groups",
                "eam:list-users",
                "eam:login",
                "eam:oidc-authorize",
                "eam:oidc-callback",
                "eam:oidc-login",
                "eam:refresh-session",
                "eam:register",
                "eam:revoke-role",
                "eam:saml-acs",
                "eam:saml-authorize",
                "eam:saml-login",
                "eam:saml-metadata",
                "eam:update-role",
                "eam:user-group-add-user",
                "eam:user-group-remove-user",

                // eap - 10 actions
                "eap:create-application",
                "eap:delete-application",
                "eap:get-application",
                "eap:get-metrics",
                "eap:list-applications",
                "eap:redeploy-application",
                "eap:set-log-level",
                "eap:start-application",
                "eap:stop-application",
                "eap:update-application",

                // ees - 6 actions
                "ees:ack-events",
                "ees:get-metrics",
                "ees:list-subscriptions",
                "ees:receive-events",
                "ees:subscribe-events",
                "ees:unsubscribe-events",

                // ekm - 14 actions
                "ekm:add-key-tag",
                "ekm:create-certificate",
                "ekm:create-key",
                "ekm:decrypt",
                "ekm:delete-certificate",
                "ekm:delete-key",
                "ekm:delete-key-tag",
                "ekm:encrypt",
                "ekm:get-certificate",
                "ekm:import-certificate",
                "ekm:list-certificates",
                "ekm:list-keys",
                "ekm:revoke-key",
                "ekm:set-key-description",

                // ekv - 10 actions
                "ekv:create-table",
                "ekv:delete-item",
                "ekv:delete-table",
                "ekv:describe-table",
                "ekv:get-item",
                "ekv:get-metrics",
                "ekv:list-tables",
                "ekv:put-item",
                "ekv:query",
                "ekv:scan",

                // emo - 3 actions
                "emo:average",
                "emo:list",
                "emo:push-metrics",

                // ens - 22 actions
                "ens:add-topic-tag",
                "ens:create-topic",
                "ens:delete-topic",
                "ens:delete-topic-tag",
                "ens:get-message-attribute",
                "ens:get-message-count",
                "ens:get-topic-ern",
                "ens:get-topic-metadata",
                "ens:list-messages",
                "ens:list-subscriptions",
                "ens:list-topics",
                "ens:publish-message",
                "ens:purge-all-topics",
                "ens:purge-topic",
                "ens:resend-messages",
                "ens:set-message-attribute",
                "ens:set-topic-max-message-length",
                "ens:set-topic-retention",
                "ens:set-topic-tag",
                "ens:start-topic",
                "ens:stop-topic",
                "ens:subscribe",
                "ens:unsubscribe",

                // eqs - 29 actions
                "eqs:add-metadata",
                "eqs:add-queue-tag",
                "eqs:create-queue",
                "eqs:delete-message",
                "eqs:delete-queue",
                "eqs:delete-queue-tag",
                "eqs:get-message-attribute",
                "eqs:get-message-count",
                "eqs:get-message-metadata",
                "eqs:get-metadata",
                "eqs:get-metrics",
                "eqs:get-queue-ern",
                "eqs:get-queue-metadata",
                "eqs:list-messages",
                "eqs:list-queues",
                "eqs:purge-all-queues",
                "eqs:purge-queue",
                "eqs:receive-messages",
                "eqs:redrive-dlq",
                "eqs:send-message",
                "eqs:set-message-attribute",
                "eqs:set-message-visibility",
                "eqs:set-queue-delay",
                "eqs:set-queue-max-message-length",
                "eqs:set-queue-tag",
                "eqs:set-queue-visibility",
                "eqs:set-visibility",
                "eqs:start-queue",
                "eqs:stop-queue",

                // esm - 37 actions
                "esm:add-bucket-tag",
                "esm:add-object-attribute",
                "esm:complete-download",
                "esm:complete-upload",
                "esm:copy-object",
                "esm:count-objects",
                "esm:create-bucket",
                "esm:create-download",
                "esm:create-upload",
                "esm:delete-bucket",
                "esm:delete-bucket-tag",
                "esm:delete-object",
                "esm:delete-object-attribute",
                "esm:delete-objects",
                "esm:disable-encryption",
                "esm:download-part",
                "esm:enable-encryption",
                "esm:get-bucket-ern",
                "esm:get-bucket-size",
                "esm:get-metrics",
                "esm:get-object",
                "esm:get-object-count",
                "esm:list-buckets",
                "esm:list-object-attributes",
                "esm:list-objects",
                "esm:list-subscriptions",
                "esm:move-object",
                "esm:purge-bucket",
                "esm:put-object",
                "esm:rename-bucket",
                "esm:rename-object",
                "esm:set-bucket-internal",
                "esm:set-bucket-tag",
                "esm:set-object-attribute",
                "esm:subscribe",
                "esm:touch-object",
                "esm:unsubscribe",
                "esm:upload-part",

                // ess - 8 actions
                "ess:add-secret-tag",
                "ess:create-secret",
                "ess:delete-secret",
                "ess:delete-secret-tag",
                "ess:get-metrics",
                "ess:get-secret",
                "ess:list-secrets",
                "ess:update-secret",

                // ets - 8 actions, plus 7 the transfer servers themselves check
                //
                // The second group is the odd one in this file: those are not x-euclid-action
                // values on a module socket but FTP verbs and SFTP packet types, checked inside
                // euclid-ftp and euclid-sftp before the command runs. They are named ets: because
                // that is the module whose servers they belong to, and a user restricted to
                // downloads is restricted by the same grant mechanism as everything else rather
                // than by a second one. PermissionVocabularyTest derives them from the two session
                // sources exactly as it derives the rest from the dispatch tables.
                "ets:create-directory",
                "ets:create-server",
                "ets:delete-directory",
                "ets:delete-file",
                "ets:delete-server",
                "ets:get-file",
                "ets:get-metrics",
                "ets:get-server",
                "ets:list-directory",
                "ets:list-servers",
                "ets:put-file",
                "ets:rename-file",
                "ets:start-server",
                "ets:stop-server",
                "ets:update-server",
        };
        return kAll;
    }

    const std::vector<std::string> &Permissions::Modules() {

        // Consecutive de-duplication is enough because All() is sorted, so a module's entries are
        // contiguous.
        static const std::vector<std::string> kModules = [] {
            std::vector<std::string> modules;
            for (const auto &permission: All()) {
                auto module = std::string(moduleOf(permission));
                if (modules.empty() || modules.back() != module) modules.push_back(std::move(module));
            }
            return modules;
        }();
        return kModules;
    }

    const std::vector<std::string> &Permissions::UnbindableModules() {

        // emm manages every module's processes and exports every collection; emd is the document
        // store, and dispatches find/insert/replace/delete against any collection by name. Granting
        // either would hand over what every other permission exists to gate. See Permissions.h.
        static const std::vector<std::string> kUnbindable = {"emd", "emm"};
        return kUnbindable;
    }

    bool Permissions::IsBindable(const std::string_view module) {
        if (std::ranges::contains(UnbindableModules(), module)) return false;
        return std::ranges::contains(Modules(), module);
    }

    std::string Permissions::Of(const std::string_view target, const std::string_view action) {
        return std::string(target) + ":" + std::string(action);
    }

    bool Permissions::Exists(const std::string_view permission) {
        return std::ranges::binary_search(All(), permission);
    }

    bool Permissions::Matches(const std::string_view granted, const std::string_view required) {

        // A required permission that is not in the vocabulary is refused whatever was granted.
        // That is what keeps the unbindable modules unreachable through here, and what makes a typo
        // in a handler fail closed rather than matching a wildcard.
        if (!Exists(required)) return false;

        if (granted == Everything) return true;
        if (granted == required) return true;

        // "<module>:*", and only in that position - "*:publish-message" is not a form this accepts,
        // because "every module's publish-message" is not something anybody should be granted.
        const auto module = moduleOf(granted);
        return granted.size() == module.size() + 2 && granted.ends_with(":*") && module == moduleOf(required);
    }

}// namespace Euclid::Core
