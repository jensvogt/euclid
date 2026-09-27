// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
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

                    // A bucket is read, written, or watched. Writing carries the multipart actions:
                    // an object over the part size is uploaded no other way, and a writer that could
                    // only put small objects would fail on exactly the files worth storing.
                    {{"buckets", "read"}, {"esm:get-bucket-ern", "esm:get-object", "esm:list-objects",
                                           "esm:list-object-attributes"}},
                    {{"buckets", "write"}, {"esm:get-bucket-ern", "esm:put-object", "esm:create-upload",
                                            "esm:upload-part", "esm:complete-upload", "esm:abort-upload",
                                            "esm:set-object-attribute", "esm:add-object-attribute"}},
                    {{"buckets", "subscribe"}, {"esm:get-bucket-ern", "esm:subscribe", "esm:unsubscribe",
                                                "esm:list-subscriptions"}},
            };

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
            for (const auto *candidate: {"produce", "consume", "read", "write", "subscribe"}) {
                if (!PermissionsFor(kind, candidate).empty()) levels.emplace_back(candidate);
            }
            return levels;
        }

        /**
         * @brief Reads one resource out of the JSON a declaration holds.
         *
         * @par
         * `access` is accepted both as a string and as an array, because both are already written by
         * hand: one bucket wants ["subscribe", "read"] and one topic wants "produce". Refusing
         * either spelling would be refusing a file somebody has already written correctly.
         *
         * @param kind  the kind this entry sits under.
         * @param value the JSON object.
         * @return the resource, or a reason it could not be read.
         */
        struct ResourceOrError {
            Resource resource;
            std::string error;
        };

        inline ResourceOrError ReadResource(const std::string &kind, const boost::json::value &value) {

            if (!value.is_object()) return {{}, kind + " entry is not an object"};
            const auto &object = value.as_object();

            Resource resource;
            resource.kind = kind;

            if (const auto *name = object.if_contains("name"); name != nullptr && name->is_string()) {
                resource.name = std::string(name->as_string());
            }
            if (resource.name.empty()) return {{}, kind + " entry has no name"};

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
                        auto read = ReadResource(kindName, entry);
                        if (!read.error.empty()) return {{}, std::string(section) + ": " + read.error};

                        if (std::string(section) == "creates") {
                            // An access level on something the application owns is not wrong so much
                            // as meaningless, and quietly ignoring it would leave somebody believing
                            // they had narrowed their own access.
                            if (!read.resource.access.empty()) {
                                return {{}, "creates: " + read.resource.name + " names an access level; owning it is the access"};
                            }
                            declaration.creates.push_back(std::move(read.resource));
                        } else {
                            if (read.resource.access.empty()) {
                                return {{}, "uses: " + read.resource.name + " names no access"};
                            }
                            for (const auto &access: read.resource.access) {
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

    }// namespace Infrastructure

}// namespace Euclid::Database::Entity::EAP
