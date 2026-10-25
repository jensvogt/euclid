// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::Database::Entity::EAP {

    /**
     * @brief What an application says it needs, and what euclid does about it.
     *
     * @par
     * An application's own repository carries a `euclid/` folder of JSON files - one naming the
     * queues and topics it owns, another the buckets somebody else owns that it has to reach. The
     * files are merged into one declaration and stored beside the artifact in the application's own
     * bucket, under the key `<application-id>.euclid.json`. EAP reads it when the application is
     * created, updated or redeployed, and makes the installation match.
     *
     * @par Why beside the artifact rather than inside it
     * A jar cannot be read without unpacking it, and a declaration euclid has to open the artifact
     * to find is one no operator can look at either. As a sibling object it is fetched, diffed and
     * versioned exactly like the artifact, by everything that already knows how to read a bucket -
     * and an application in any language carries it the same way.
     *
     * @par The two halves
     * `creates` is what the application owns: euclid makes these, and they belong to it. `uses` is
     * what somebody else owns: euclid grants access to them and creates nothing, because a service
     * that could conjure another team's queue by naming it would make ownership meaningless.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace Infrastructure {

        /**
         * @brief The key the declaration is stored under, beside the artifact.
         *
         * @par
         * Derived rather than configured, so that nothing has to be told where to look: an
         * application's id is what euclid already has in hand everywhere this is read.
         *
         * @param applicationId the application's id.
         * @return the object key, e.g. "file-copy.euclid.json".
         */
        inline std::string ObjectKey(const std::string &applicationId) {
            return applicationId + ".euclid.json";
        }

        /**
         * @brief The kinds of resource a declaration can name.
         *
         * @par
         * Spelled as they appear in the file - plural, because that is how the JSON reads and a
         * mismatch between the two would be a silent no-op rather than an error.
         */
        constexpr std::array kResourceKinds{
                std::string_view{"queues"},
                std::string_view{"topics"},
                std::string_view{"buckets"},
                // Only ever under `uses`. A secret's value cannot travel in a declaration that
                // ships inside the artifact, so euclid does not create one; naming it is how an
                // application's grant comes to cover the secrets it reads and no others. Read()
                // refuses one under `creates` saying so.
                std::string_view{"secrets"},
                // An EKV table: made with the key its file names, which can never change after.
                std::string_view{"tables"},
                // An EAG route publishing a path for this application. Only ever under `creates`:
                // a route is published by the application it reaches.
                std::string_view{"routes"},
        };

        /**
         * @brief One resource the application owns, or one it only reaches.
         */
        struct Resource {

            /**
             * @brief "queues", "topics" or "buckets".
             */
            std::string kind;

            /**
             * @brief The resource's name, unqualified. Resolved against the application's own
             * account and namespace - a declaration names a resource the way a person does.
             */
            std::string name;

            /**
             * @brief What the application may do with it, for a `uses` entry. Empty for `creates`,
             * where owning it is the access.
             */
            std::vector<std::string> access;

            /**
             * @brief Which application owns it, for a `uses` entry. Carried for the reader rather
             * than acted on: euclid checks the resource exists, not who wrote it down.
             */
            std::string owner;

            /**
             * @brief How a `creates` entry wants the object made: every field but `name`, as
             * written. Empty for `uses`, where nothing is made.
             *
             * @par
             * Checked against SettingsFor() when the file is read, so a key the kind does not have
             * fails there and names the file. It used to be dropped instead - on the deploy path
             * every setting was, so `"visibility": 900` came up as 30 with nothing saying so.
             */
            boost::json::object settings;

            bool operator==(const Resource &) const = default;
        };

        /**
         * @brief A whole declaration, as one file or as several merged.
         */
        struct Declaration {

            /**
             * @brief Schema version. Only 1 exists; anything else is refused rather than guessed at.
             */
            long version{};

            /**
             * @brief Which application this declaration belongs to, if it says.
             *
             * @par
             * The guard against a declaration being applied to the wrong application. Without it the
             * only thing tying a file to an application is the object key it was stored under, so a
             * mistyped name - or any path that copies one application's record to another - hands one
             * service's queues and topics to a second, which then deletes them on its next reconcile
             * because they are recorded and no longer declared. That is not a hypothetical: it is what
             * happened, and it took a topic and a queue with it.
             *
             * @par
             * Empty is allowed, because a declaration written before this field existed has no claim
             * to make and refusing those would break what is already deployed. Present and wrong is
             * refused - see Belongs().
             */
            std::string applicationId;

            /**
             * @brief What the application owns and euclid should make.
             */
            std::vector<Resource> creates;

            /**
             * @brief What somebody else owns and the application should be able to reach.
             */
            std::vector<Resource> uses;
        };

        /**
         * @brief The permissions one access level grants on one kind of resource.
         *
         * @par
         * A fixed table rather than the built-in roles, because a role is broader than an access
         * level: `consumer` can set a message's visibility on any queue it is granted, and an
         * application that only produces has no business doing that. Each entry here is the smallest
         * set that makes the named verb work, and it is granted narrowed to the one resource ERN.
         *
         * @par
         * `get-queue-ern` and `get-topic-ern` appear in almost every row, and have to: a name is not
         * an ERN, and every other action takes the ERN. An access level that could not resolve the
         * name it was granted on would be granted on nothing.
         *
         * @param kind   "queues", "topics" or "buckets".
         * @param access "produce", "consume", "read", "write" or "subscribe".
         * @return the permissions, or empty when the pair is not one this table knows.
         */
        /**
         * @brief The access an application has on what it creates: everything the kind has.
         *
         * @par
         * Granted by EAP on each owned object, so owning it is the access - literally, rather than
         * by the grace of an `application` grant on "*". Without it an application could delete,
         * move or rename nothing in its own bucket.
         */
        inline constexpr auto kOwnAccess = "own";

        inline std::vector<std::string> PermissionsFor(const std::string &kind, const std::string &access) {

            static const std::map<std::pair<std::string, std::string>, std::vector<std::string> > kTable{
                    // A queue is produced to and consumed from, and the two are deliberately not the
                    // same grant: the service that fills a queue and the service that drains it are
                    // usually different, and each reaching only its own end is the point.
                    {{"queues", "produce"}, {"eqs:get-queue-ern", "eqs:send-message", "eqs:send-message-batch"}},
                    {{"queues", "consume"}, {"eqs:get-queue-ern", "eqs:receive-messages", "eqs:delete-message",
                                             "eqs:set-message-visibility", "eqs:get-message-count"}},

                    // A topic is published to or subscribed to. Subscribing carries unsubscribe and
                    // the listing, because a listener that cannot see whether it is already
                    // subscribed subscribes twice on every restart.
                    {{"topics", "produce"}, {"ens:get-topic-ern", "ens:publish-message"}},
                    {{"topics", "subscribe"}, {"ens:get-topic-ern", "ens:subscribe", "ens:unsubscribe",
                                               "ens:list-subscriptions"}},

                    // A bucket is read, written, or watched. Both directions carry the multipart
                    // actions: an object over the part size is uploaded no other way, and one over
                    // the inline limit is downloaded no other way - get-object answers it 413 and
                    // the SDK's downloadFile goes on to create-download. A reader or a writer that
                    // could only handle small objects would fail on exactly the files worth
                    // storing.
                    {{"buckets", "read"}, {"esm:get-bucket-ern", "esm:get-object", "esm:get-object-size", "esm:list-objects",
                                           "esm:list-object-attributes", "esm:create-download",
                                           "esm:download-part", "esm:complete-download"}},
                    {{"buckets", "write"}, {"esm:get-bucket-ern", "esm:put-object", "esm:create-upload",
                                            "esm:upload-part", "esm:complete-upload", "esm:abort-upload",
                                            "esm:set-object-attribute", "esm:add-object-attribute"}},
                    {{"buckets", "subscribe"}, {"esm:get-bucket-ern", "esm:subscribe", "esm:unsubscribe",
                                                "esm:list-subscriptions"}},

                    // The destructive ones, each its own level rather than folded into write: a
                    // service that drops files into somebody's bucket has no business removing what
                    // others put there. delete-objects is the by-key form, and what an SDK's
                    // deleteObject calls when it has a key rather than an ERN.
                    {{"buckets", "delete"}, {"esm:get-bucket-ern", "esm:delete-object", "esm:delete-objects"}},
                    // Copy and move together: a move is a copy and a delete, and is what a large
                    // delivery has to use - a copy of 12 GB never finishes, a move is a row edit.
                    // ESM checks a move against both buckets, so it is granted on both ends: declare
                    // "move" on the bucket objects come from and on the one they go to.
                    {{"buckets", "move"}, {"esm:get-bucket-ern", "esm:copy-object", "esm:move-object"}},
                    {{"buckets", "rename"}, {"esm:get-bucket-ern", "esm:rename-object"}},

                    // Stopping and starting a queue, which a feature flag uses to switch a consumer
                    // off without undeploying it. Separate from consume: the service that reads a
                    // queue and the one allowed to switch it off are rarely the same.
                    {{"queues", "control"}, {"eqs:get-queue-ern", "eqs:stop-queue", "eqs:start-queue"}},

                    // A secret is read, and that is the whole of it. One permission and no ERN
                    // lookup beside it: unlike every other kind here, ess:get-secret takes the
                    // secret's *name*, which is what configuration can carry and what stays the
                    // same between environments - so there is no name-to-ERN step for an access
                    // level to have to grant. list-secrets stays out deliberately, because what
                    // an application needs is the value of a secret it was told the name of, not
                    // the names of the ones it was not.
                    {{"secrets", "read"}, {"ess:get-secret"}},

                    // A table is read or written. Writing carries delete-item, because replacing and
                    // removing an item are the same decision about somebody's data - put-item already
                    // replaces whatever was there. get-table is in both: an SDK reads the key schema
                    // before it can build a key.
                    {{"tables", "read"}, {"ekv:get-table", "ekv:get-item", "ekv:query", "ekv:scan"}},
                    {{"tables", "write"}, {"ekv:get-table", "ekv:put-item", "ekv:delete-item"}},
            };

            // What owning means: every level the kind has, granted on the object itself. Not a
            // level anybody declares - `creates` is how an application says it owns something -
            // and not in the table, so that it cannot drift from the levels it is made of.
            if (access == kOwnAccess) {
                std::set<std::string> all;
                for (const auto &[key, permissions]: kTable) {
                    if (key.first == kind && key.first != "secrets") all.insert(permissions.begin(), permissions.end());
                }
                return {all.begin(), all.end()};
            }

            const auto it = kTable.find({kind, access});
            return it != kTable.end() ? it->second : std::vector<std::string>{};
        }

        /**
         * @brief Every access level this table knows for a kind, for an error message worth reading.
         *
         * @param kind "queues", "topics" or "buckets".
         * @return the levels, sorted, or empty for a kind that is not one of the three.
         */
        inline std::vector<std::string> AccessLevelsFor(const std::string &kind) {
            std::vector<std::string> levels;
            for (const auto *candidate: {"produce", "consume", "control", "read", "write", "subscribe", "delete", "move", "rename"}) {
                if (!PermissionsFor(kind, candidate).empty()) levels.emplace_back(candidate);
            }
            return levels;
        }

        /**
         * @brief What is wrong with a resource name, or nothing.
         *
         * @par
         * Not each module's own rules - those stay in the module - but enough that a name which
         * cannot work is reported against the file holding it rather than as a module error later.
         */
        inline std::optional<std::string> NameProblem(const std::string &name) {
            constexpr std::size_t kMaxNameLength = 255;
            if (name.empty()) return "has an empty name";
            if (name.size() > kMaxNameLength) return "has a name longer than " + std::to_string(kMaxNameLength) + " characters";
            // An ERN names an account and a namespace, and a declaration is applied into whichever
            // namespace the application is deployed to. Accepting one would be a file that reaches
            // into production from a development deployment.
            if (name.starts_with("ern:")) return "is an ERN; name it alone and let the deployment's namespace decide";
            if (std::ranges::any_of(name, [](const unsigned char c) { return std::isspace(c) != 0; })) return "has whitespace in its name";
            return std::nullopt;
        }

        /**
         * @brief One setting a `creates` entry may carry.
         */
        struct SettingRule {
            std::string_view key;
            /// Whole number between minimum and maximum; one of `choices`; a resource name; an
            /// attribute name (an EKV key); a path starting with "/"; or a list of HTTP methods.
            enum class Type { Integer, Choice, Name, Attribute, Path, Methods } type;
            long minimum{};
            long maximum{};
            std::vector<std::string_view> choices;
            /// Has to be given: a table without a key, or a route without a path, means nothing.
            bool required = false;
        };

        /**
         * @brief The settings each kind accepts under `creates`.
         *
         * @par
         * A list here rather than passed through to the module, which is what the CLI path used to
         * do: the module ignored a key it did not know, so a misspelt `"visibilty"` was a queue at
         * the default and a file that looked right. Named as the euclid-pim files already write
         * them.
         *
         * @par
         * Buckets have none yet. Encryption is the one setting a bucket has, and it names a key
         * that has to exist first - a separate piece of work rather than a field to pass along.
         */
        inline const std::vector<SettingRule> &SettingsFor(const std::string &kind) {
            using T = SettingRule::Type;
            constexpr long kDay = 24L * 60 * 60;
            static const std::vector<SettingRule> kQueues{
                    // At least one second: 0 makes a received message visible again at once, and a
                    // second consumer is handed it while the first is still working on it.
                    {"visibility", T::Integer, 1, 12 * 60 * 60, {}},
                    // Receives before a message goes to the dead letter queue.
                    {"maxRetries", T::Integer, 1, 1000, {}},
                    {"maxMessageLength", T::Integer, 1, 256L * 1024 * 1024, {}},
                    {"delay", T::Integer, 0, 12 * 60 * 60, {}},
                    // Seconds a message may wait; 0 is the installation's default.
                    {"retention", T::Integer, 0, 3650 * kDay, {}},
                    {"priority", T::Choice, 0, 0, {"LOW", "MEDIUM", "HIGH"}},
                    // Another queue's name, in this declaration or already there.
                    {"deadLetterQueue", T::Name, 0, 0, {}},
            };
            static const std::vector<SettingRule> kTopics{
                    {"maxMessageLength", T::Integer, 1, 256L * 1024 * 1024, {}},
                    {"retention", T::Integer, 0, 3650 * kDay, {}},
            };
            // Fixed for the table's life: a key cannot change once items are keyed on it, so a file
            // that names a different one than the table has is reported, not applied.
            static const std::vector<SettingRule> kTables{
                    {"partitionKey", T::Attribute, 0, 0, {}, true},
                    {"partitionKeyType", T::Choice, 0, 0, {"string", "number", "binary"}},
                    {"sortKey", T::Attribute, 0, 0, {}},
                    {"sortKeyType", T::Choice, 0, 0, {"string", "number", "binary"}},
            };
            // A route publishing a path for this application - the application is always the one
            // declaring it. Upload routes and routes to a euclid module stay with "eag create-route".
            static const std::vector<SettingRule> kRoutes{
                    {"path", T::Path, 0, 0, {}, true},
                    {"methods", T::Methods, 0, 0, {}},
                    {"authentication", T::Choice, 0, 0, {"NONE", "EUCLID", "BASIC"}},
                    {"timeout", T::Integer, 0, 3600, {}},
            };
            static const std::vector<SettingRule> kNone;
            if (kind == "queues") return kQueues;
            if (kind == "topics") return kTopics;
            if (kind == "tables") return kTables;
            if (kind == "routes") return kRoutes;
            return kNone;
        }

        /**
         * @brief What is wrong with a `creates` entry's settings, or nothing.
         */
        inline std::optional<std::string> SettingsProblem(const std::string &kind, const Resource &resource) {

            const auto &rules = SettingsFor(kind);
            for (const auto &[key, value]: resource.settings) {
                const auto rule = std::ranges::find(rules, std::string_view(key), &SettingRule::key);
                if (rule == rules.end()) {
                    std::string allowed;
                    for (const auto &known: rules) allowed += (allowed.empty() ? "" : ", ") + std::string(known.key);
                    return resource.name + ": " + kind + " have no setting \"" + std::string(key) + "\""
                           + (allowed.empty() ? " - they take none" : " - only " + allowed);
                }
                const auto where = resource.name + ": \"" + std::string(key) + "\"";
                switch (rule->type) {
                    case SettingRule::Type::Integer:
                        if (!value.is_int64() || value.as_int64() < rule->minimum || value.as_int64() > rule->maximum) {
                            return where + " has to be a whole number from " + std::to_string(rule->minimum) + " to " + std::to_string(rule->maximum);
                        }
                        break;
                    case SettingRule::Type::Choice: {
                        const auto chosen = value.is_string() ? std::string_view(value.as_string()) : std::string_view{};
                        if (!std::ranges::contains(rule->choices, chosen)) {
                            std::string allowed;
                            for (const auto &choice: rule->choices) allowed += (allowed.empty() ? "" : ", ") + std::string(choice);
                            return where + " has to be one of " + allowed;
                        }
                        break;
                    }
                    case SettingRule::Type::Name:
                        if (!value.is_string()) return where + " has to be a name";
                        if (const auto problem = NameProblem(std::string(value.as_string()))) return where + " " + *problem;
                        if (value.as_string() == resource.name) return where + " names the queue itself";
                        break;
                    case SettingRule::Type::Attribute: {
                        // EKV's own rule for a key attribute - see Value::IsAcceptableAttributeName.
                        const auto name = value.is_string() ? std::string_view(value.as_string()) : std::string_view{};
                        if (name.empty() || name.starts_with('$') || name.find('.') != std::string_view::npos) {
                            return where + " has to be an attribute name - not empty, not starting with '$', without '.'";
                        }
                        break;
                    }
                    case SettingRule::Type::Path:
                        if (!value.is_string() || !value.as_string().starts_with("/")) return where + " has to be a path starting with '/'";
                        break;
                    case SettingRule::Type::Methods: {
                        static const std::set<std::string, std::less<> > kMethods{"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
                        if (!value.is_array()) return where + " has to be a list of HTTP methods";
                        for (const auto &method: value.as_array()) {
                            auto upper = method.is_string() ? std::string(method.as_string()) : std::string{};
                            std::ranges::transform(upper, upper.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
                            if (!kMethods.contains(upper)) return where + ": " + boost::json::serialize(method) + " is not an HTTP method";
                        }
                        break;
                    }
                }
            }
            for (const auto &rule: rules) {
                if (rule.required && !resource.settings.contains(rule.key)) {
                    return resource.name + ": a " + kind.substr(0, kind.size() - 1) + " needs \"" + std::string(rule.key) + "\"";
                }
            }
            // The pairs: a sort key's type without a sort key is a key nobody asked for, and the two
            // halves of a key being one attribute is no key at all.
            if (kind == "tables") {
                if (resource.settings.contains("sortKeyType") && !resource.settings.contains("sortKey")) {
                    return resource.name + ": \"sortKeyType\" without a \"sortKey\"";
                }
                if (const auto *sort = resource.settings.if_contains("sortKey"); sort != nullptr && *sort == resource.settings.at("partitionKey")) {
                    return resource.name + ": the partition key and the sort key have to be different attributes";
                }
            }
            return std::nullopt;
        }

        /**
         * @brief Reads one resource out of the JSON a declaration holds.
         *
         * @par
         * `access` is accepted both as a string and as an array, because both are already written by
         * hand: one bucket wants ["subscribe", "read"] and one topic wants "produce". Refusing
         * either spelling would be refusing a file somebody has already written correctly.
         *
         * @param kind    the kind this entry sits under.
         * @param value   the JSON object.
         * @param section "creates" or "uses": a `uses` entry is named and reached, not configured,
         *                so anything but name, access and owner is refused there; under `creates`
         *                everything but the name is a setting, checked by SettingsProblem().
         * @return the resource, or a reason it could not be read.
         */
        struct ResourceOrError {
            Resource resource;
            std::string error;
        };

        inline ResourceOrError ReadResource(const std::string &kind, const boost::json::value &value,
                                            const std::string_view section = "uses") {

            if (!value.is_object()) return {{}, kind + " entry is not an object"};
            const auto &object = value.as_object();

            Resource resource;
            resource.kind = kind;

            if (const auto *name = object.if_contains("name"); name != nullptr && name->is_string()) {
                resource.name = std::string(name->as_string());
            }
            if (resource.name.empty()) return {{}, kind + " entry has no name"};
            if (const auto problem = NameProblem(resource.name)) return {{}, resource.name + " " + *problem};

            if (section == "creates") {
                // Access is refused by Read(), which says why; everything else is a setting.
                for (const auto &[key, field]: object) {
                    if (key == "name" || key == "access") continue;
                    resource.settings[key] = field;
                }
                if (const auto problem = SettingsProblem(kind, resource)) return {{}, *problem};
            } else {
                for (const auto &field: object) {
                    if (field.key() == "name" || field.key() == "access" || field.key() == "owner") continue;
                    return {{}, resource.name + " has an unknown field \"" + std::string(field.key())
                                        + "\"; an object named under uses is reached, not configured"};
                }
            }

            if (const auto *owner = object.if_contains("owner"); owner != nullptr && owner->is_string()) {
                resource.owner = std::string(owner->as_string());
            }

            if (const auto *access = object.if_contains("access"); access != nullptr) {
                if (access->is_string()) {
                    resource.access.emplace_back(access->as_string());
                } else if (access->is_array()) {
                    for (const auto &entry: access->as_array()) {
                        if (!entry.is_string()) return {{}, resource.name + ": access entries have to be strings"};
                        resource.access.emplace_back(entry.as_string());
                    }
                } else {
                    return {{}, resource.name + ": access has to be a string or an array of strings"};
                }
            }

            return {resource, {}};
        }

        /**
         * @brief Reads one declaration file.
         *
         * @param value the parsed JSON.
         * @return the declaration, or a reason it could not be read.
         */
        struct DeclarationOrError {
            Declaration declaration;
            std::string error;
        };

        inline DeclarationOrError Read(const boost::json::value &value) {

            if (!value.is_object()) return {{}, "declaration is not a JSON object"};
            const auto &object = value.as_object();

            Declaration declaration;

            // Refused rather than defaulted. A file with no version is one written against a schema
            // nobody has decided yet, and reading it as version 1 would apply a guess to somebody's
            // installation.
            if (const auto *version = object.if_contains("version"); version != nullptr && version->is_int64()) {
                declaration.version = version->as_int64();
            }
            if (const auto *owner = object.if_contains("applicationId"); owner != nullptr && owner->is_string()) {
                declaration.applicationId = std::string(owner->as_string());
            }

            // Refused rather than skipped: a misspelt "create" that is quietly ignored is a deploy
            // that comes up having made nothing, with every line of the file looking right.
            for (const auto &field: object) {
                const auto key = std::string(field.key());
                if (key == "version" || key == "applicationId" || key == "creates" || key == "uses") continue;
                return {{}, "unknown section \"" + key + "\"; expected creates or uses"};
            }

            if (declaration.version != 1) {
                return {{}, R"(version has to be 1, not ")" + boost::json::serialize(object.if_contains("version") != nullptr
                                                                                             ? *object.if_contains("version")
                                                                                             : boost::json::value{}) + R"(")"};
            }

            for (const auto *section: {"creates", "uses"}) {
                const auto *sectionValue = object.if_contains(section);
                if (sectionValue == nullptr) continue;
                if (!sectionValue->is_object()) return {{}, std::string(section) + " is not an object"};

                for (const auto &[kind, entries]: sectionValue->as_object()) {
                    const auto kindName = std::string(kind);
                    if (!std::ranges::contains(kResourceKinds, kindName)) {
                        return {{}, std::string(section) + ": unknown resource kind \"" + kindName + "\""};
                    }
                    if (!entries.is_array()) return {{}, kindName + " has to be an array"};

                    for (const auto &entry: entries.as_array()) {
                        auto read = ReadResource(kindName, entry, section);
                        if (!read.error.empty()) return {{}, std::string(section) + ": " + read.error};

                        if (std::string(section) == "creates") {
                            // Refused rather than attempted. Creating a secret means writing its
                            // value, and the only place a declaration could carry one is the file
                            // itself - which ships in the application's artifact. An operator
                            // writes it once; the declaration says which ones are read.
                            if (kindName == "secrets") {
                                return {{}, "creates: " + read.resource.name + " is a secret; a secret's value cannot travel in a "
                                                                               "declaration, so it is written once with \"ess create-secret\" and named under \"uses\""};
                            }
                            // An access level on something the application owns is not wrong so much
                            // as meaningless, and quietly ignoring it would leave somebody believing
                            // they had narrowed their own access.
                            if (!read.resource.access.empty()) {
                                return {{}, "creates: " + read.resource.name + " names an access level; owning it is the access"};
                            }
                            declaration.creates.push_back(std::move(read.resource));
                        } else {
                            if (kindName == "routes") {
                                return {{}, "uses: " + read.resource.name + " is a route; a route is published by the application it "
                                                                            "reaches, so it is declared under creates by that one"};
                            }
                            if (read.resource.access.empty()) {
                                return {{}, "uses: " + read.resource.name + " names no access"};
                            }
                            for (const auto &access: read.resource.access) {
                                if (access == kOwnAccess) {
                                    return {{}, "uses: " + read.resource.name + ": \"own\" is what creates means; "
                                                        "an object somebody else owns is reached, not owned"};
                                }
                                if (PermissionsFor(kindName, access).empty()) {
                                    auto levels = AccessLevelsFor(kindName);
                                    std::string allowed;
                                    for (const auto &level: levels) allowed += (allowed.empty() ? "" : ", ") + level;
                                    return {{}, "uses: " + read.resource.name + ": " + kindName + " have no \"" + access
                                                        + "\" access - only " + allowed};
                                }
                            }
                            declaration.uses.push_back(std::move(read.resource));
                        }
                    }
                }
            }

            return {declaration, {}};
        }

        /**
         * @brief Whether this declaration may be applied to that application.
         *
         * @par
         * A declaration that names no application belongs to whichever one it was stored beside, which
         * is how every file written before the field existed behaves. One that names a different
         * application is refused rather than applied: the resources it creates would be recorded
         * against the wrong owner, and the owner's next reconcile would delete them.
         *
         * @param declaration   the declaration.
         * @param applicationId the application it is about to be applied to.
         * @return empty to go ahead, or the reason not to.
         */
        inline std::string Belongs(const Declaration &declaration, const std::string &applicationId) {
            if (declaration.applicationId.empty() || declaration.applicationId == applicationId) return {};
            return "declaration belongs to \"" + declaration.applicationId + "\", not to \"" + applicationId + "\"";
        }

        /**
         * @brief Merges the files of one `euclid/` folder into the declaration that is stored.
         *
         * @par
         * The split across files is the author's business - one named its file topics.json and put a
         * queue in it - so merging is by section and kind rather than by filename. Two files naming
         * the same resource is refused rather than resolved: whichever of them a merge preferred, the
         * other author would have been overruled silently.
         *
         * @param declarations the files, in whatever order they were read.
         * @return one declaration, or the reason they cannot be merged.
         */
        inline DeclarationOrError Merge(const std::vector<Declaration> &declarations) {

            Declaration merged;
            merged.version = 1;

            // One claim, or none. Two files in a folder naming different applications is refused
            // rather than resolved by taking the last: silently picking one is how a declaration ends
            // up attributed to an application that does not own it, which is the failure this field
            // was added to prevent in the first place.
            for (const auto &declaration: declarations) {
                if (declaration.applicationId.empty()) continue;
                if (!merged.applicationId.empty() && merged.applicationId != declaration.applicationId) {
                    return {{}, "these files disagree about whose they are: \"" + merged.applicationId
                                        + "\" and \"" + declaration.applicationId + "\""};
                }
                merged.applicationId = declaration.applicationId;
            }

            std::set<std::pair<std::string, std::string> > seen;

            for (const auto &declaration: declarations) {
                for (const auto &resource: declaration.creates) {
                    if (!seen.insert({resource.kind, resource.name}).second) {
                        return {{}, "declared twice: " + resource.kind + " \"" + resource.name + "\""};
                    }
                    merged.creates.push_back(resource);
                }
                for (const auto &resource: declaration.uses) {
                    if (!seen.insert({resource.kind, resource.name}).second) {
                        return {{}, "declared twice: " + resource.kind + " \"" + resource.name + "\""};
                    }
                    merged.uses.push_back(resource);
                }
            }

            // Sorted so that the stored object is the same bytes for the same folder however the
            // files were listed - which is what lets a redeploy tell "unchanged" from "rewritten".
            const auto byKindThenName = [](const Resource &a, const Resource &b) {
                return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
            };
            std::ranges::sort(merged.creates, byKindThenName);
            std::ranges::sort(merged.uses, byKindThenName);

            return {merged, {}};
        }

        /**
         * @brief The declaration as the sidecar object holds it.
         *
         * @param declaration what to write.
         * @return the JSON, in the shape Read() accepts.
         */
        inline boost::json::value Write(const Declaration &declaration) {

            const auto section = [](const std::vector<Resource> &resources) {
                boost::json::object out;
                for (const auto &resource: resources) {
                    boost::json::object entry{{"name", resource.name}};
                    for (const auto &[key, value]: resource.settings) entry[key] = value;
                    if (!resource.owner.empty()) entry["owner"] = resource.owner;
                    if (!resource.access.empty()) {
                        boost::json::array access;
                        for (const auto &level: resource.access) access.push_back(boost::json::value(level));
                        entry["access"] = access;
                    }
                    if (!out.contains(resource.kind)) out[resource.kind] = boost::json::array{};
                    out[resource.kind].as_array().push_back(entry);
                }
                return out;
            };

            boost::json::object out{{"version", declaration.version}};
            if (!declaration.applicationId.empty()) out["applicationId"] = declaration.applicationId;
            if (!declaration.creates.empty()) out["creates"] = section(declaration.creates);
            if (!declaration.uses.empty()) out["uses"] = section(declaration.uses);
            return out;
        }

        /**
         * @brief An application's `euclid/` folder, read and merged.
         */
        struct FolderOrError {
            Declaration declaration;
            std::string error;
            /// Whether the folder was there at all. Absent is not an error - most applications
            /// declare nothing - so the caller decides what it means.
            bool present{};
            std::size_t files{};
        };

        /**
         * @brief Reads every *.json in a folder and merges them into one declaration.
         *
         * @par
         * The one way a folder is read, by every command that reads one. There used to be two
         * readers, one for `eap apply` and one for the deploy, and they disagreed about what a
         * file could say: the first passed a queue's settings on, the second dropped them.
         *
         * @param directory the folder.
         * @return the merged declaration, or the first problem, naming its file.
         */
        inline FolderOrError LoadFolder(const std::filesystem::path &directory) {

            FolderOrError result;
            std::error_code ec;
            if (!std::filesystem::is_directory(directory, ec)) return result;
            result.present = true;

            // Name order, so the same folder reads the same way twice and an error about one file
            // names the same file next time.
            std::vector<std::filesystem::path> files;
            for (const auto &entry: std::filesystem::directory_iterator(directory, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
            }
            std::ranges::sort(files);
            result.files = files.size();

            std::vector<Declaration> declarations;
            for (const auto &file: files) {
                const auto name = file.filename().string();
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    result.error = name + ": could not be opened";
                    return result;
                }
                std::ostringstream buffer;
                buffer << in.rdbuf();

                // A byte order mark, which Windows editors write by default and JSON does not allow.
                // Skipped: it is invisible, and "not JSON" about an invisible character helps nobody.
                std::string_view text = buffer.view();
                if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

                boost::system::error_code parseError;
                const auto parsed = boost::json::parse(text, parseError);
                if (parseError) {
                    result.error = name + ": not valid JSON: " + parseError.message();
                    return result;
                }
                auto read = Read(parsed);
                if (!read.error.empty()) {
                    result.error = name + ": " + read.error;
                    return result;
                }
                declarations.push_back(std::move(read.declaration));
            }

            auto merged = Merge(declarations);
            if (!merged.error.empty()) {
                result.error = merged.error;
                return result;
            }
            result.declaration = std::move(merged.declaration);
            return result;
        }

    }// namespace Infrastructure

}// namespace Euclid::Database::Entity::EAP
