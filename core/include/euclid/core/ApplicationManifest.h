// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::Core {

    /**
     * @brief What one application says it needs from an installation.
     *
     * @par Why an application says it at all
     * Today a deployment says nothing about what it will use, so EAP grants its technical principal
     * the `application` role across every resource in the namespace - `resources = ["*"]` - because
     * that is the only safe reading of silence. The queues, topics and buckets themselves are then
     * created by hand, or by a script somebody keeps, or by the application on first run; and the
     * roles that narrow access to them are written by an operator per application, which is what
     * the `pim-object-writer`, `pim-queue-consumer` and `pim-bucket-subscriber` roles in a running
     * installation are. None of that is anywhere near the code it describes.
     *
     * @par
     * A manifest is that list, shipped with the application in a `euclid/` directory and read at
     * deploy. It gives an installation three things it cannot otherwise have: the objects exist
     * before the first instance starts, the principal is granted exactly them rather than
     * everything, and what an application touches can be read without running it.
     *
     * @par creates and uses
     * The two halves are not interchangeable. @ref ApplicationManifest::creates is what this
     * application owns - it is the producer, and euclid makes the object and records the owner.
     * @ref ApplicationManifest::uses is what it consumes from somebody else: nothing is created, the
     * object must already exist, and a deploy that cannot find it fails naming the application that
     * should have created it. Ownership following the producer is what makes a shared object have
     * exactly one definition, and what makes pruning safe - an application's own objects are the
     * only ones it can ever remove.
     *
     * @par This is a request, not a grant
     * An application's artifact is supplied by whoever built it, which for a delivery pipeline is
     * frequently not the operator. A manifest therefore asks; EAP decides, and only ever within
     * what the `application` role already allows. Nothing here can widen a principal beyond that
     * ceiling, and anything outside it is an operator's decision at deploy time rather than a line
     * in a file that arrived with a jar.
     *
     * @author jensvogt47\@gmail.com
     */
    struct ApplicationManifest {

        /**
         * @brief How an application reaches an object it does not own.
         *
         * @par
         * Kept coarse on purpose. These map to permissions the `application` role already holds,
         * and the point of naming one is to ask for fewer than all of them - a finer vocabulary
         * here would be a second permission model to keep in step with the first.
         */
        enum class Access : std::uint8_t {
            Read,     ///< list and get. Objects out of a bucket, nothing written back.
            Write,    ///< read, plus put and delete. What a producer needs on somebody else's bucket.
            Consume,  ///< receive and delete messages from a queue somebody else created.
            Produce,  ///< send messages to a queue, or publish to a topic.
            Subscribe,///< attach a queue of one's own to a topic or a bucket's events.
        };

        /**
         * @brief What kind of object a declaration is about.
         *
         * @par Why Secret is only ever used, never created
         * The other three are objects an application owns and euclid can make for it. A secret is
         * the one whose *value* is the point, and that value cannot travel in a manifest: the
         * manifest ships in the artifact, so a `creates` entry for a secret would mean database
         * credentials committed to the application's own repository. Writing one is an operator's
         * job, done once with `euclid-cli ess create-secret`; the manifest's part is to say which
         * of them this application reads, so the grant can name those and not every secret in the
         * namespace. @ref ParseApplicationManifest refuses a secret under `creates` saying so, and
         * `read` is the only access level it accepts for one.
         */
        enum class Kind : std::uint8_t { Bucket, Queue, Topic, Secret };

        /**
         * @brief One object this application owns and euclid should create.
         */
        struct Creates {
            Kind kind{};
            std::string name;

            /**
             * @brief Settings for the object, passed through to the module that makes it.
             *
             * @par
             * Deliberately opaque here: a queue's visibility timeout and a bucket's encryption key
             * are EQS's and ESM's business, and a manifest parser that knew about every one of them
             * would need changing whenever either module gained a setting. Unknown ones are the
             * module's to reject, at the point where the rule about them actually lives.
             */
            boost::json::object settings;

            /**
             * @brief Which file it was declared in, for an error message that can be acted on.
             */
            std::string source;
        };

        /**
         * @brief One object this application consumes and somebody else owns.
         */
        struct Uses {
            Kind kind{};
            std::string name;
            /**
             * @brief Every way this application reaches the object, at least one.
             *
             * @par
             * A list because one object is routinely reached two ways at once: a @BucketListener
             * attaches to a bucket's events and then fetches the object each event names, which is
             * subscribe and read. Declaring it twice is still refused - two declarations of one
             * object are two answers - so the ways it is reached belong in one of them.
             */
            std::vector<Access> access{Access::Read};

            /**
             * @brief The application expected to own it, when the author knows.
             *
             * @par
             * Optional, and only ever used to say something useful when the object is missing:
             * "queue 'parsing-in' is used but does not exist - it is created by 'parsing'" is a
             * sentence somebody can act on, where "does not exist" is a support call. Not checked
             * against the actual owner, which would turn a stale comment into a failed deploy.
             */
            std::string owner;

            std::string source;
        };

        /**
         * @brief Manifest format version. Only 1 exists.
         */
        int version{1};

        std::vector<Creates> creates;
        std::vector<Uses> uses;

        /**
         * @brief Whether this manifest asks for anything at all.
         */
        [[nodiscard]] bool empty() const { return creates.empty() && uses.empty(); }
    };

    /**
     * @brief A manifest, or why there isn't one.
     *
     * @par
     * Errors are collected rather than thrown one at a time: a manifest is authored by hand, and
     * somebody fixing it wants every problem in the directory at once rather than one per deploy.
     * Each message names the file it came from, because the manifest is several files merged and
     * "duplicate queue 'parsing-in'" without them is a hunt.
     */
    struct ManifestResult {
        ApplicationManifest manifest;
        std::vector<std::string> errors;

        [[nodiscard]] bool ok() const { return errors.empty(); }
    };

    /**
     * @brief Reads and merges every manifest file in an application's euclid/ directory.
     *
     * @par Why a directory rather than a file
     * A repository holding twenty services has twenty of these, and the ones that matter are edited
     * by different people for different reasons - a queue added here, a bucket there. One file per
     * concern in a directory keeps those apart; merging them means the application still has a
     * single list.
     *
     * @par
     * Every `*.json` directly in the directory is read, sorted by name so that the same directory
     * always merges the same way, and subdirectories are ignored rather than walked - a `euclid/`
     * directory is a small set of declarations, and anything nested in it is somebody's notes.
     *
     * @param directory the application's euclid/ directory.
     * @return the merged manifest, or the errors that stopped it. A directory that does not exist
     * is not an error and yields an empty manifest: most applications declare nothing, and a deploy
     * must not start refusing them.
     */
    [[nodiscard]] ManifestResult LoadApplicationManifest(const std::filesystem::path &directory);

    /**
     * @brief Parses one manifest document, for callers that already have the text.
     *
     * @param json the document.
     * @param source what to call it in an error message.
     */
    [[nodiscard]] ManifestResult ParseApplicationManifest(const std::string &json, const std::string &source);

    /**
     * @brief The wire spelling of an access level, as a manifest writes it.
     */
    [[nodiscard]] std::string ToString(ApplicationManifest::Access access);

    /**
     * @brief Several of them, comma separated, as a message names them.
     */
    [[nodiscard]] std::string ToString(const std::vector<ApplicationManifest::Access> &access);

    /**
     * @brief The wire spelling of an object kind, singular - "bucket", "queue", "topic".
     */
    [[nodiscard]] std::string ToString(ApplicationManifest::Kind kind);

}// namespace Euclid::Core
