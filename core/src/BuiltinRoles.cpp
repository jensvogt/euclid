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

        // Moved to Permissions::IsRead() so that EAD, which records the actions this one does not
        // match, cannot drift from the role that grants the ones it does.
        bool isRead(const std::string_view permission) {
            return Permissions::IsRead(permission);
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
                        // The same two operations again, for a file too big to move in one call.
                        // TransferStorage picks the path by size, not by permission: above the part
                        // size an upload becomes create-upload/upload-part/complete-upload and a
                        // download becomes create-download/download-part/complete-download. Holding
                        // only the single-shot pair meant a transfer user could store a small file
                        // and not a large one - and the refusal arrives at CLOSE, after the client
                        // has sent every byte, as "Could not store object" with nothing to say
                        // which of the two paths it was on.
                        //
                        // No wider than put-object and get-object already are: these move the same
                        // bytes to the same key in the same bucket, in pieces.
                        "esm:complete-download",
                        "esm:complete-upload",
                        "esm:create-download",
                        "esm:create-upload",
                        "esm:download-part",
                        "esm:upload-part",
                };
                std::ranges::sort(permissions);
                return permissions;
            }();
            return kPermissions;
        }

        // Exactly the calls euclid-wrk makes as itself - WorkerClient's four EAP actions and the four
        // ESM ones Core::Artifact fetches with - and nothing an operator does to a node.
        const std::vector<std::string> &nodePermissions() {
            static const std::vector<std::string> kPermissions = [] {
                std::vector<std::string> permissions{
                        "eap:issue-instance-credentials",
                        "eap:register-node",
                        "eap:renew-node",
                        "eap:report-node-instance",
                        "esm:complete-download",
                        "esm:create-download",
                        "esm:download-part",
                        "esm:get-object",
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
                                "eap:report-load",
                                // How deep the queues it polls are, which is the other figure in
                                // that report and the only one that can grow a listener's pool.
                                // Utilisation cannot: it is measured against the sum of every
                                // listener's concurrency, so an instance whose one busy queue is
                                // saturated still reports a fraction of capacity and never reaches
                                // the saturation bar. Backlog is what evaluateScaling() turns into
                                // a desired count, and it is counted a queue at a time through
                                // eqs:get-message-count.
                                //
                                // Worse than a refusal, left out: euclid-spring's listener
                                // container counts best-effort, so a queue it may not count
                                // contributes nothing and the 403 goes to debug. The report still
                                // arrives, carrying backlog 0, and a pool with ten thousand
                                // messages in front of it looks exactly like an idle one.
                                //
                                // Not in consumerPermissions(), where the rest of what a listener
                                // needs lives: counting is not part of consuming, and a principal
                                // granted the consumer role to read one queue should not thereby
                                // learn how much is waiting in it. This is here because the
                                // container asks for it on every tick of every deployed
                                // application.
                                "eqs:get-message-count",
                                // Who its caller is, for an application behind EAG that treats
                                // euclid as its identity provider. The gateway states the caller in
                                // x-euclid-user-id, and an application that maps euclid groups onto
                                // its own roles has to read the groups to do it - without this the
                                // lookup is refused, the application cannot authorise anybody, and
                                // every request through the gateway is answered 401 by the
                                // application rather than by euclid.
                                //
                                // Reading only, and the calling account's own groups: nothing here
                                // creates a group, changes a membership, or reads a user. An
                                // application that can see which groups exist is a long way from
                                // one that can put itself in them.
                                "eam:list-user-groups",
                                // Its own metrics, for the history and the dashboards. A different
                                // question from the load report above and not a substitute for it:
                                // EMO writes a row when its averaging bucket closes, five minutes
                                // as shipped, so an autoscaler driven from here reacts minutes
                                // late - which is why utilisation was moved to eap:report-load.
                                // What EMO is for is what happened over the last fortnight.
                                //
                                // Withheld until now, which made "the application goes on pushing
                                // the same numbers to EMO" - the arrangement EmoServer and
                                // EapServer both describe - something no application could
                                // actually do: every push from a technical principal was answered
                                // 403. An SDK still reporting the old way (euclid-spring's
                                // listener container) does not degrade to the new one, it just
                                // fails every fifteen seconds.
                                //
                                // What it costs, stated plainly: push-metrics takes the module
                                // label from the request body, so an application holding this can
                                // write samples labelled as anything. That is metric pollution and
                                // nothing more - nothing reads EMO to make a decision, the
                                // autoscaler least of all, which is exactly why it stopped.
                                "emo:push-metrics",
                                // Its own configuration. An application reads the credentials of
                                // the database it talks to while its context is being built, which
                                // is the whole reason ESS exists and is as much a part of starting
                                // up as the delivery queue below - an application that cannot do it
                                // does not start, it throws out of the first datasource it tries to
                                // make. Withholding it made ESS unreachable by anything deployed:
                                // no application role held a single ess permission, so every read
                                // was answered 403 before ESS saw the request.
                                //
                                // Reading only, and one secret at a time by the name the deployment
                                // configured. The write actions stay out - a principal that can
                                // rotate or delete the credentials it reads is a different thing
                                // from one that uses them - and so does list-secrets, because
                                // enumerating an account's secret names is reconnaissance an
                                // application has no use for. get-secret is resource-checked
                                // (EssServer::denyUngrantedSecret), so a manifest naming secrets
                                // narrows this to exactly the ones declared; a deployment that
                                // named nothing keeps the ["*"] it already had everywhere else.
                                "ess:get-secret"});

                built[std::string(BuiltinRoles::Transfer)] = transferPermissions();
                built[std::string(BuiltinRoles::Node)] = nodePermissions();

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
        if (name == Node) return "What a euclid-wrk node does as itself: register, renew, take its instances' credentials, report on them, and fetch their artifacts";
        if (name == Transfer) return "Everything an FTP or SFTP client can do: list, download, upload, rename, create and remove directories, including the bucket objects those become";
        return {};
    }

}// namespace Euclid::Core
