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
#include <euclid/database/entity/RuntimeName.h>
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
         * @brief Name identifying this application within its account and namespace.
         *
         * What a person deploys, names and asks for: unique within (accountId, nameSpace), like
         * every other named resource. Not what the application runs as - see @ref runtimeName.
         */
        std::string applicationId;

        /**
         * @brief The name the manager runs this application under, issued once and never changed.
         *
         * @par
         * Everything outside the definition is keyed by this: the process pool, the module row the
         * manager registers, the data directory, the unix socket, the log channel and the
         * technical principal named after it. None of those has an account or a namespace to live
         * in, so none of them can be keyed by an applicationId, which is unique only within one.
         *
         * @par
         * Stored rather than derived so that it holds still. An application that moves to another
         * namespace keeps its directory, its module row, its principal and its processes; only
         * where it looks resources up changes, which is the only thing that should.
         *
         * @par
         * Empty on applications deployed before this field existed. They ran under their bare
         * applicationId, and RuntimeName() goes on returning exactly that for them, so nothing
         * about them moves. Written only when set - see toDocument() - so that the unique index on
         * this field skips them rather than seeing every one of them as the same empty name.
         */
        std::string runtimeName;

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
         * @brief Namespace this application belongs to, and whose resources it works with.
         *
         * @par
         * Part of the application's identity: an applicationId is unique within
         * (accountId, nameSpace), and the ERN carries all three. It is also where the application
         * *works* - the namespace its queue, topic and bucket names are resolved in, sent as
         * x-euclid-namespace on every call it makes.
         *
         * @par
         * Empty on every application created before this existed, and on any created by a client
         * that does not set it. The manager then falls back to the single namespace the
         * application's EAM user is granted, which is unambiguous for an application deployed the
         * ordinary way - see writeApplicationCredentials().
         */
        std::string nameSpace;

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
         * @brief Level this application's own output is logged at, or empty to leave it to the
         * configuration.
         *
         * @par
         * An application writes to standard output and standard error, and the manager reads that
         * back and logs it on the application's own channel ("app.<applicationId>"). This is that
         * channel's level, kept with the application rather than in a configuration file so it can
         * be changed while everything is running - see the EAP module's set-log-level action.
         * "off" silences the application entirely.
         *
         * @par
         * Deliberately not part of what the manager treats as a change of definition: a level is
         * not a reason to restart a running application, so it is stored on its own and applied on
         * the next reconcile without touching @ref modified.
         */
        std::string logLevel;

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
     * @brief The name an application runs under, as opposed to the name it is defined under.
     *
     * @par
     * An applicationId is unique within (accountId, nameSpace), like every other named resource.
     * Its runtime identity cannot be: the manager keys its process pools by name, registers each
     * as an EMM module (whose names are installation-wide), gives each a data directory, a unix
     * socket and a log channel, and not one of those has an account or a namespace to live in.
     *
     * @par
     * So it is a name of its own - @ref Application::runtimeName, issued once by
     * GenerateRuntimeName() and then never touched. Deriving it from the account, namespace and id
     * instead would make it change whenever they do: moving an application to another namespace
     * would move its directory, rename its module row and its principal, and bounce it, for a
     * change that has nothing to do with how it runs.
     *
     * @param application the application
     * @return the name to run it under
     */
    std::string RuntimeName(const Application &application);

    // An application's runtime name is issued by Entity::GenerateRuntimeName() - see
    // euclid/database/entity/RuntimeName.h, which a transfer server uses for the same purpose.

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
     * A deployment is supposed to move an application from one build to another, and there is one
     * way that silently fails to happen: the artifact is byte for byte the one already deployed,
     * so the restart it causes changes nothing. That is refused, and this is the single place that
     * decides it - the CLI asks before uploading, the eap module asks before storing, and the
     * answer has to be the same one.
     *
     * @par
     * The version is not part of the decision. Deploying the same version with different bytes is
     * ordinary - a rebuilt snapshot, a fix that keeps the number - and it is the bytes that say
     * whether anything is actually being deployed. `deployedVersion` is still taken, because the
     * refusal names it when it can.
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
