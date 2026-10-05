// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <WorkerReconciler.h>
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/ArtifactFetcher.h>

namespace Euclid::Worker {

    /**
     * @brief What this worker was started with.
     */
    struct Options {

        /**
         * @brief The gateway to reach euclid through, e.g. "https://euclid.example:5566".
         */
        std::string endpoint;

        /**
         * @brief What this node calls itself. Defaults to the host name.
         */
        std::string nodeName;

        /**
         * @brief Labels an application's placement constraints are matched against.
         */
        std::map<std::string, std::string> labels;

        /**
         * @brief Where artifacts, credentials and logs go.
         */
        std::string dataDir;

        /**
         * @brief How often to renew and reconcile.
         */
        std::chrono::seconds tick{10};

        /**
         * @brief A CA certificate to trust in addition to the system store, for an installation
         * with a self-signed gateway.
         */
        std::string caCertPath;

        /**
         * @brief The IP address this node reports when it registers.
         *
         * @par
         * Empty means found out at registration time: the local address of the route to the
         * master, which is the address the master's network can reach this node on. Set it when
         * that is not true - a node behind NAT, say - since nothing on this side can know that.
         */
        std::string address;
    };

    /**
     * @brief This worker's conversation with the master, and the processes it has.
     *
     * @par
     * A euclid client rather than a module: it reaches the gateway over HTTP with signed requests,
     * holds a role, and has no MongoDB credentials, no EMD and nothing listening that anything can
     * connect to. See docs/worker-nodes.md §2.
     *
     * @par
     * Four calls out and one decision in. The decision is Reconciler::Decide(), which is kept
     * separate and pure because it is the whole safety argument; this type is the plumbing that
     * feeds it and carries out what it says.
     *
     * @author jensvogt47\@gmail.com
     */
    class WorkerClient {

    public:

        explicit WorkerClient(Options options, CLI::Credentials::Entry credentials);

        /**
         * @brief Announces this node. Called once at start-up, and again after a renewal that said
         * the node is not registered.
         *
         * @return the lease period the master granted, or nothing if registration failed.
         */
        [[nodiscard]] std::optional<std::chrono::seconds> Register();

        /**
         * @brief Renews every lease and asks what this node should be running.
         *
         * @par
         * One call, because the heartbeat and the poll are the same thing: there is no way to be
         * heartbeating and not reconciling.
         *
         * @param assigned written with the desired set when this succeeds.
         * @param leaseExpiresAt written with the new deadline when this succeeds.
         * @return true when the renewal succeeded. False covers both "could not reach the master"
         * and "the master refused", which the caller treats alike - it keeps running until its own
         * deadline passes either way.
         */
        [[nodiscard]] bool Renew(std::vector<Reconciler::Assignment> &assigned,
                                 std::chrono::system_clock::time_point &leaseExpiresAt);

        /**
         * @brief Does what a plan says: fetches and starts, stops and reaps, and reports each.
         */
        void Apply(const Reconciler::Plan &plan);

        /**
         * @brief What this worker currently has running, for the next decision.
         */
        [[nodiscard]] std::vector<Reconciler::Running> Running() const;

        /**
         * @brief Replaces the credentials of any running instance that is halfway through theirs.
         *
         * @par
         * Called every tick. This is why the credentials are a file rather than an environment
         * variable: an instance started an hour ago is holding a token that is about to expire,
         * and rewriting the file is the only thing that can replace it without restarting the
         * process. The manager has always done this for the applications it runs; a worker has to
         * do it for the ones it runs, or every placed application stops working one TTL after it
         * starts.
         */
        void RefreshCredentials();

        /**
         * @brief Stops everything, reporting each as it goes. What a lease running out means, and
         * also what an orderly shutdown does.
         */
        void StopAll();

    private:

        /**
         * @brief One process this worker started.
         */
        struct Instance {
            Reconciler::Assignment assignment;
            int pid{-1};
            int httpPort{};

            /**
             * @brief When this instance's credentials should be asked for again.
             *
             * @par
             * Halfway through their life, which is the rule the manager already follows for the
             * applications it runs itself (`credentialsNeedRefresh`): an application then always
             * has at least half a lifetime in hand, so a few failed refreshes in a row cost
             * nothing. Derived from the blob's own expiry rather than from a configured TTL,
             * because the TTL is the master's setting and a worker has no business holding a copy
             * of it - it would only be a second place for the two to disagree.
             */
            std::chrono::system_clock::time_point credentialsRefreshAt{};
        };

        /**
         * @brief A POST that cannot throw.
         *
         * @par
         * CLI::HttpClient throws when it cannot reach the endpoint, and an unreachable master is
         * the one condition this worker exists to survive: the lease is there precisely so that
         * losing contact is handled rather than fatal. An exception escaping to main() would take
         * the process down and leave its instances orphaned - still running, unsupervised, and
         * holding a lease nobody will renew, which is the exact state §5 is built to make
         * impossible.
         *
         * @par
         * Status 0 for "never got that far", the same convention
         * Core::ModuleClient::ModuleResponse uses, so a caller cannot confuse an unreachable
         * master with a refusal.
         */
        [[nodiscard]] CLI::HttpResponse post(const std::string &target, const std::string &action,
                                             const boost::json::value &body) const;

        /**
         * @brief How this worker reaches ESM to fetch an artifact: as a signed client through the
         * gateway, which is the half of Core::Artifact that differs from the manager's.
         */
        [[nodiscard]] Core::Artifact::Call artifactTransport() const;

        [[nodiscard]] bool start(const Reconciler::Assignment &assignment);
        void stop(const Reconciler::Running &running);
        void report(const Reconciler::Assignment &assignment, int pid, int httpPort, const std::string &state) const;

        /**
         * @brief Asks the master for one instance's credentials and writes them where the process
         * will look.
         *
         * @par
         * The master mints and the worker receives. A worker must never hold the signing secret -
         * anything holding it can mint a token for any principal in the installation, including
         * admin, offline and unloggably, so distributing it would make every worker host a full
         * compromise of the control plane. §3.2.
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point>
        writeCredentials(const Reconciler::Assignment &assignment) const;

        [[nodiscard]] std::filesystem::path applicationDir(const std::string &runtimeName) const;

        Options _options;
        CLI::Credentials::Entry _credentials;
        CLI::HttpClient _client;

        /**
         * @brief What this worker has started, by instance id.
         */
        std::map<std::string, Instance> _instances;
    };

}// namespace Euclid::Worker
