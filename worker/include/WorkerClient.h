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
#include <euclid/core/ApplicationLaunch.h>
#include <euclid/core/ArtifactFetcher.h>
#include <euclid/core/SystemUtils.h>

namespace Euclid::Worker {

    /**
     * @brief Where this node keeps each interpreter: euclid.worker.runtimes.&lt;key&gt;.
     *
     * @par
     * A path per host, because an application names a version and not a location - the JDKs are
     * under /usr/lib/jvm on one host and somewhere else on the next, and this is how a node says
     * which. JAVA also still reads euclid.worker.java, the name every worker configured before the
     * others existed uses; the newer key wins where both are set.
     *
     * @par
     * One definition rather than a lambda at each use, so what a runtime means when an application
     * is started is the same thing CheckRuntimes() reported on at startup.
     */
    [[nodiscard]] Core::Launch::Interpreter RuntimeLookup();

    /**
     * @brief Logs a warning for every runtime this node could not start, if any.
     *
     * @par
     * Called once at startup. An interpreter this node was told nothing about, or the wrong path
     * for, is otherwise found out only when an application is placed here and exits 127 - which is
     * a message about the configuration arriving long after it was written, in the log of whatever
     * application happened to need it.
     */
    void CheckRuntimes();

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
         * @brief Notices the instances whose process exited on its own, and gives each up.
         *
         * @par
         * Called every tick, before Running(), so the next decision sees what is actually alive. An
         * exited instance is reaped - it would otherwise stay a zombie - reported to the master as
         * CRASHED, and held off from restarting by Reconciler::RestartDelay(). Without this a
         * process that died went on being counted as running, kept having its credentials
         * refreshed, showed as RUNNING to the master, and was never started again.
         */
        void Reap();

        /**
         * @brief What this worker currently has running, for the next decision.
         */
        [[nodiscard]] std::vector<Reconciler::Running> Running() const;

        /**
         * @brief The crashed slots not yet due to be started again - see Reconciler::State::holdOff.
         */
        [[nodiscard]] std::map<std::string, std::chrono::system_clock::time_point> HoldOff() const;

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
             * @brief What was exec'd, kept for the one message that needs it.
             *
             * @par
             * An instance that exits 127 was never executed at all - most often an interpreter
             * this node was told the wrong path for, since euclid.worker.runtimes.java25 is a path
             * per host and an application only names the version. "the command could not be
             * executed" without saying which command leaves the reader to guess at exactly the
             * moment the answer is one string.
             */
            std::string executable;

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

            /**
             * @brief When the process was started, so a crash can say how long it ran - which is
             * what decides whether the backoff grows or starts again.
             */
            std::chrono::system_clock::time_point startedAt{};

            /**
             * @brief On Windows, the process and its stop event - see Core::WindowsProcess::Spawned.
             * A pid is not something to wait on or signal there. Held as void * so this header
             * does not pull <windows.h> into everything that includes it; both are HANDLEs, closed
             * when the instance is stopped or reaped. Unused elsewhere.
             */
            void *processHandle{};
            void *stopEvent{};
        };

        /**
         * @brief A slot whose process exited on its own: the delay its restart was held off by,
         * and when that runs out.
         *
         * @par
         * Kept past the restart, so a slot that crashes again straight away has its delay doubled
         * rather than started over - and dropped once the slot is no longer assigned here.
         */
        struct Crash {
            std::chrono::seconds delay{};
            std::chrono::system_clock::time_point notBefore{};
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

        /**
         * @brief Asks an instance to stop - SIGTERM to its process group, or its stop event on
         * Windows - without waiting. Safe to ask twice.
         */
        void askToStop(const Instance &instance);

        /**
         * @brief Stops an instance: asks, waits until @p deadline, then kills. Reaps and reports it.
         */
        void stop(const Reconciler::Running &running, std::chrono::steady_clock::time_point deadline);
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

        /**
         * @brief Holds each application's output channel at the level eap set-log-level gave it,
         * as the manager does for the applications it runs - see Reconciler::Assignment::logLevel.
         */
        void applyLogLevels(const std::vector<Reconciler::Assignment> &assigned);

        Options _options;
        CLI::Credentials::Entry _credentials;
        CLI::HttpClient _client;

        /**
         * @brief What this worker has started, by instance id.
         */
        std::map<std::string, Instance> _instances;

        /**
         * @brief Slots whose process exited on its own, by instance id.
         */
        std::map<std::string, Crash> _crashes;

        /**
         * @brief The HTTP port each slot was given, by instance id - see Core::Launch::PickHttpPort.
         *
         * @par
         * Held for as long as the slot is assigned here, not only while its process runs, so a
         * restart after a crash lands on the same port and the gateway's backend does not move.
         */
        std::map<std::string, int> _ports;

        /**
         * @brief The level applied to each application output channel, so an unchanged one is not
         * applied again every tick; empty for "the configuration's".
         */
        std::map<std::string, std::string> _channelLevels;

        /**
         * @brief The address this node registered with, which is what it reports as an instance's
         * host - the gateway connects to it, and an address needs no DNS where a host name does.
         */
        std::string _address;

        /**
         * @brief The previous Core::SystemUtils::ReadCpuTimes() reading, for the percentage sent
         * with the next renewal.
         *
         * @par
         * CPU usage is a delta between two readings, so the first renewal after a start only
         * primes this and reports nothing. Held per client rather than in a file-local because
         * ReadCpuTimes() is deliberately stateless: two pollers sharing one baseline would each
         * read a figure for an interval neither of them measured.
         */
        std::optional<Core::SystemUtils::CpuTimes> _previousCpuTimes;
    };

}// namespace Euclid::Worker
