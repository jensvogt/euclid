//
// Created by vogje01 on 9/1/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/eap/ApplicationState.h>
#include <euclid/database/entity/eap/Runtime.h>

namespace Euclid::Database::Entity::EAP {

    using std::chrono::system_clock;

    /**
     * @brief One deployed application: a process euclid runs on behalf of a user.
     *
     * @par
     * The definition is the whole contract between the modules it ties together, the same way a
     * transfer server's is. EAP owns it and is the only thing that writes it; euclid-mgr reads it
     * to decide which processes to run, materialises the artifact out of ESM and hands the
     * process its credentials; and the application itself only ever sees the environment those
     * two agreed on.
     *
     * @par The artifact lives in ESM
     * An application is not a path on a host: it is an object in a bucket, named here by bucket
     * ERN and key. That is what makes a deployment a normal upload - through the CLI, an SDK, or
     * an FTP transfer server - and what lets a manager on a fresh host bring an application up
     * with nothing but the database and the object store.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Application {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Name identifying this application, unique across the installation.
         *
         * Doubles as the manager's module name for the spawned processes, which is why it has to
         * be unique: two applications sharing a name would share a process pool.
         */
        std::string applicationId;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Account this application belongs to
         */
        std::string accountId;

        /**
         * @brief Region this application belongs to
         */
        std::string region;

        /**
         * @brief Runtime the artifact is started with - see RuntimeCommandPrefix().
         */
        Runtime runtime = Runtime::UNKNOWN;

        /**
         * @brief ERN of the bucket holding the artifact.
         */
        std::string bucketErn;

        /**
         * @brief Key of the artifact object within that bucket, e.g. "apps/orders-1.4.jar".
         */
        std::string artifactKey;

        /**
         * @brief Version of the build currently deployed, e.g. "1.4.0".
         *
         * @par
         * Given at creation or read out of the artifact's own name, and changed only by a
         * redeploy - which is refused unless it changes. That is what makes this worth storing:
         * without it "which build is running?" can only be answered by looking at a checksum, and
         * two deployments of the same version are indistinguishable after the fact.
         */
        std::string version;

        /**
         * @brief MD5 of the artifact object as it was when this version was deployed.
         *
         * @par
         * Copied from the ESM object rather than computed here, so it is the same hash the
         * manager compares against when it decides whether the copy on the host is still the
         * build it should be running. A redeploy whose artifact hashes the same is the same
         * build - a version bump that shipped nothing - and is refused.
         */
        std::string md5Sum;

        /**
         * @brief Command to run instead of the runtime's default.
         *
         * @par
         * Empty for the usual case, where the runtime and the artifact are enough ("java -jar
         * <artifact>"). Set when an application needs something else entirely - a wrapper script,
         * an interpreter that isn't on PATH, a module invocation rather than a file.
         */
        std::string command;

        /**
         * @brief Arguments passed after the artifact path.
         */
        std::vector<std::string> arguments;

        /**
         * @brief Environment variables handed to the process, on top of the EUCLID_* ones the
         * manager injects.
         */
        std::map<std::string, std::string> environment;

        /**
         * @brief ERNs of the buckets and queues this application may act on.
         *
         * @par
         * Mirrored onto the technical principal's resource grants, which is what the storage and
         * queueing modules actually enforce. Empty means unrestricted within the application's
         * account - the state stage one left every application in, and still what an application
         * that names no resources gets.
         */
        std::vector<std::string> resources;

        /**
         * @brief EAM user the application runs as.
         *
         * @par
         * Its access key is what the process signs its own calls back into euclid with, so this
         * is the identity euclid sees when the application talks to ESM, EQS or anything else -
         * not the identity of whoever deployed it.
         */
        std::string userId;

        /**
         * @brief Smallest number of instances the autoscaler keeps running.
         */
        long minInstances = 1;

        /**
         * @brief Largest number of instances the autoscaler may scale out to.
         */
        long maxInstances = 1;

        /**
         * @brief How long an instance may take to create its socket before the manager gives up
         * on it, in milliseconds.
         *
         * @par
         * Generous by default because this is where language runtimes differ most: a Rust binary
         * is listening in milliseconds, a JVM with a framework on top can take ten seconds.
         */
        long readyTimeoutMs = 30000;

        /**
         * @brief What the application should be doing - see ApplicationState.
         */
        ApplicationState desiredState = ApplicationState::STOPPED;

        /**
         * @brief Creation date
         */
        system_clock::time_point created = system_clock::now();

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified = system_clock::now();

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        static Application fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

    /**
     * @brief Reads a version out of an artifact's own name.
     *
     * @par
     * Builds carry their version in their file name - orders-1.4.0.jar,
     * file-copy-service-2.0.11-SNAPSHOT.jar - so in the ordinary case nobody has to repeat what
     * the name already says. The first "x.y.z" in it is taken, which is what a person reads off
     * it too.
     *
     * @par
     * A name that says nothing about its build ("orders.jar") gets an empty answer rather than a
     * guess, and the caller is asked for a version instead: a recorded version that was invented
     * is worse than none.
     *
     * @param name an artifact key or a local file name
     * @return the version, or empty if the name does not carry one
     */
    std::string VersionFromArtifactName(const std::string &name);

    /**
     * @brief Why deploying this build over the deployed one would not be a deployment, or empty
     * if it would.
     *
     * @par
     * A deployment is supposed to move an application from one build to another, and there are
     * two ways that silently fails to happen: the version does not change, so nothing afterwards
     * can say which build is running; or the artifact is byte for byte the one already deployed,
     * so the restart it causes changes nothing. Both are refused, and this is the single place
     * that decides it - the CLI asks before uploading, the eap module asks before storing, and
     * the answer has to be the same one.
     *
     * @param deployedVersion version currently recorded on the application, empty for one defined
     * before versions existed
     * @param deployedMd5Sum artifact checksum currently recorded, empty for the same reason
     * @param version version being deployed
     * @param md5Sum checksum of the build being deployed
     * @return the reason to refuse, phrased for whoever is deploying, or empty to go ahead
     */
    std::string RedeployRefusal(const std::string &deployedVersion, const std::string &deployedMd5Sum,
                                const std::string &version, const std::string &md5Sum);

}// namespace Euclid::Database::Entity::EAP
