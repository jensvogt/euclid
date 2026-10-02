// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 10/22/23.
//

#pragma once

// C++ includes
#include <string>
#include <string_view>
#include <chrono>
#include <vector>

// Mongo Db includes
#include <bsoncxx/builder/basic/document.hpp>

// Euclid includes
#include <euclid/database/entity/emm/ModuleState.h>

namespace Euclid::Database::Entity {

    /**
     * @brief One running (or pending-restart/crashed) instance of a module's autoscaled pool.
     *
     * Mirrors ServiceController's in-memory pool 1:1: one ModuleInstance per live slot, updated
     * in place across restarts (matched by instanceId, which - unlike pid - stays the same for
     * the life of the slot) and removed only when the slot itself is permanently gone (autoscaler
     * scale-down, or the manager giving up on it after exceeding its module's maxRestarts).
     */
    struct ModuleInstance {

        /**
         * @brief Stable identifier for this pool slot, unchanged across restarts. The key used
         * to find/replace this entry within its module's `instances` array.
         */
        std::string instanceId;

        /**
         * @brief Process ID of the running instance, or -1 while not running.
         *
         * @par
         * Only ever meaningful together with @ref host. A pid identifies a process on one machine
         * and nothing at all on any other, so every piece of code that acts on this - signalling
         * it, reading /proc for it, deciding it is a leftover - has to establish that the record
         * is about the machine it is running on. @ref isOn is that question.
         */
        int pid = -1;

        /**
         * @brief Which machine this instance is on. Empty means the host reading it.
         *
         * @par
         * A Module document is keyed by module name, so every host running a module called "esm"
         * shares one document and its instances all sit in the same array. Without this field
         * there is nothing in a record to say whose process it describes, and a manager sweeping
         * leftovers on start-up will find a live local pid that matches another host's record -
         * same installation path, same executable, pids colliding freely across machines - and
         * kill a healthy process on the strength of it.
         *
         * @par Why empty means "mine" rather than "unknown"
         * Every record written before this field existed has no host, and an installation that
         * upgrades must not have its own instances suddenly read as somebody else's: they would be
         * neither swept nor managed, and would survive as orphans the manager no longer touches.
         * Reading an absent host as the local one keeps a single-host installation behaving
         * exactly as it did, with no migration.
         *
         * @par
         * The cost of that choice is the mirror image: on a multi-host installation a record
         * written by an older manager is claimed by whichever host looks first. That is the
         * transitional case, it ends as soon as every manager writes the field, and it is strictly
         * better than the current behaviour, where *every* record is claimed by every host.
         */
        std::string host;

        /**
         * @brief Whether this record describes a process on the given host.
         *
         * @param hostName the machine asking, normally SystemUtils::GetHostName().
         * @return true when the record names that host, or names none at all.
         */
        [[nodiscard]] bool isOn(const std::string_view hostName) const noexcept {
            return host.empty() || host == hostName;
        }

        /**
         * @brief Which node has been told to run this slot, as distinct from where it is running.
         *
         * @par
         * Empty means the manager's own host, which is every instance on an installation with no
         * workers. The difference from @ref host is the whole point of having both: `assignedTo`
         * is a decision the master made and `host` is a fact a worker reported, and between the
         * two is the window where a slot has been given to a node that has not started it yet -
         * or has stopped it and not yet said so.
         */
        std::string assignedTo;

        /**
         * @brief When the node's claim on this slot stops being valid.
         *
         * @par
         * The safety argument in docs/worker-nodes.md §5, and the reason assignment is a lease
         * rather than a flag. The obvious design - the master notices a worker has gone quiet and
         * starts the instances somewhere else - is the classic way to end up running two of
         * something that must only run once, because a worker that lost its connection has not
         * necessarily lost its processes. The network broke; the JVM is still consuming the queue.
         *
         * @par
         * So the guarantee runs the other way. The worker renews on every tick, and **a worker
         * that cannot renew stops its instances itself** - not when it decides the master is gone,
         * but when its own lease runs out, which is a local clock against a local deadline and
         * needs nobody's agreement. The master re-places a slot only after this has passed, plus a
         * margin for clock skew.
         *
         * @par
         * What that buys: at any moment after expiry, either the worker has already stopped the
         * instances or the worker is not executing at all. Both are safe to re-place on top of.
         * What it costs is an outage of one lease period for work on a partitioned worker, which
         * is the right trade for a queue consumer and the wrong one for anything that must never
         * stop - see §11.
         *
         * @par
         * Zero means no lease was ever written, which is an unassigned slot on the manager's own
         * host rather than one whose lease has lapsed. @ref leaseHasExpired says so.
         */
        std::chrono::system_clock::time_point leaseExpiresAt{};

        /**
         * @brief Whether this slot's lease has run out, by the asking clock.
         *
         * @par
         * False for a slot that never had one - an instance the manager runs itself holds no
         * lease, and reading "no deadline" as "deadline passed" would make every ordinary instance
         * look re-placeable.
         *
         * @param margin added to the deadline before it counts as passed, for clock skew between
         * the node that wrote it and whoever is asking.
         * @param now the clock to compare against.
         */
        [[nodiscard]] bool leaseHasExpired(const std::chrono::seconds margin = std::chrono::seconds{0},
                                           const std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const noexcept {
            if (leaseExpiresAt.time_since_epoch().count() == 0) return false;
            return now > leaseExpiresAt + margin;
        }

        /**
         * @brief Whether this slot is the given node's to run.
         *
         * @par
         * Empty `assignedTo` is the manager's own, for the reason an empty @ref host is: it is
         * what every record written before workers existed carries, and what an installation
         * running no workers writes today.
         */
        [[nodiscard]] bool isAssignedTo(const std::string_view nodeName) const noexcept {
            return assignedTo.empty() ? nodeName.empty() : assignedTo == nodeName;
        }

        /**
         * @brief Current lifecycle state of this instance.
         */
        ModuleState state = ModuleState::STOPPED;

        /**
         * @brief Full path to this instance's own (pid-suffixed) Unix domain socket, or empty
         * while not running.
         */
        std::string socketPath;

        /**
         * @brief TCP port this instance was given for its own HTTP listener, or 0 for none.
         *
         * @par
         * Several instances of one application run on the same host, so a port written into the
         * application's own configuration would be bound by the first instance and refused to
         * every other - which is not a failure the autoscaler can see, only a pool that will not
         * grow. The manager hands each instance a port of its own instead, the same way it hands
         * each one a socket path of its own.
         *
         * @par
         * Persisted because it is the only record of which instance is reachable where: an API
         * gateway in front of an application's web interface has no other way to find its
         * backends, and the manager itself forgets everything when it restarts.
         */
        int httpPort{};

        /**
         * @brief Number of times this slot has been restarted since its last stable run.
         */
        int restartCount{};

        /**
         * @brief Work this instance is doing that no request is waiting on, as it reports it.
         *
         * @par
         * Written by the module, not by the manager - it is the one thing the manager cannot see.
         * An `--async` purge is answered at once and carried on afterwards on a thread, so
         * acquireInstance()/releaseInstance() have long since put the instance back to idle while
         * the work runs. The autoscaler used to stop exactly such an instance, and the removal
         * went with it.
         *
         * @par
         * Not a load signal: it says "do not stop me", not "start another one". Work like this is
         * not served any faster by a second instance.
         *
         * @par Deliberately absent from toDocument()
         * (The same is true of the three load fields below, for the same reason.)
         *
         * @par
         * Read here and written nowhere in this file. The module writes it on its own with a
         * targeted update - IEmmRepository::reportBackgroundTasks() - because the module is the
         * only thing that can count its own threads, and everything else in this entity belongs to
         * the manager.
         *
         * @par Two writers, one per kind of pool
         * A euclid module writes it through reportBackgroundTasks(); a deployed application writes
         * it through its load report, IEmmRepository::reportInstanceLoad(), because an application
         * has no database to reach. They never contend: an application pool's record is only ever
         * reported for by the application, and a module's only by the module. What an application
         * counts is handlers it has started and not finished - a bucket listener mid-message is
         * work a second instance cannot take over, which is exactly what this field is for.
         *
         * @par
         * Being absent from toDocument() does not by itself protect it. The manager used to persist
         * an instance with `$set: {"instances.$": <the whole subdocument>}`, which replaces the
         * array element and so destroyed every field the replacement did not carry - this one
         * included, on every state change. MongoEmmRepository::upsertInstance() now sets the fields
         * the manager owns one at a time, which is what keeps the two writers out of each other's
         * way. Adding this to toDocument() would hand ownership back.
         */
        long backgroundTasks{};

        /**
         * @brief How loaded this instance says it is, 0-100, or -1 when it has never said.
         *
         * @par Why this is not read from EMO
         * It was. An application pushed `application-utilisation` as a metric and the manager read
         * it back out of the monitoring store - which accumulates samples in memory and writes a
         * row only when its averaging bucket closes, every `euclid.modules.emo.average-period`
         * seconds. So a figure reported every fifteen seconds reached the autoscaler up to five
         * minutes later, and scaling was late by that much in both directions.
         *
         * @par
         * Utilisation is a control signal, not a measurement to graph. The monitoring store is
         * built to aggregate for cheap retention, and shortening its bucket to serve a control
         * loop would cost twenty times the rows for every metric euclid keeps, to fix one. So the
         * signal comes here instead, on the record the manager already reads every reconcile, and
         * EMO goes on doing what it is for - the application reports to both.
         */
        double utilisation = -1.0;

        /**
         * @brief How much work is waiting that this instance has not started, or -1 when unknown.
         *
         * @par
         * The other half of the question. Utilisation alone cannot tell "working through a burst,
         * nearly done" from "cannot keep up" - both read as busy - and only the instance knows how
         * deep its own queues are, since a bucket listener's delivery queue is named after the run
         * that created it.
         */
        long backlog = -1;

        /**
         * @brief When this instance last reported, or the epoch if it never has.
         *
         * @par
         * Without it a dead reporter's last figure would stand for ever, and the autoscaler would
         * hold a pool at whatever load the application had when it stopped talking. The reader
         * treats anything older than a few reporting intervals as "not reporting" rather than as
         * "idle" - see ServiceController::reconcileApplicationLoad().
         */
        std::chrono::system_clock::time_point loadReportedAt{};

        /**
         * @brief Time this instance entry was first persisted.
         */
        std::chrono::system_clock::time_point created = std::chrono::system_clock::now();

        /**
         * @brief Time this instance entry was last updated.
         */
        std::chrono::system_clock::time_point modified = std::chrono::system_clock::now();

        /**
         * @brief Converts this instance to a BSON (sub)document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Builds a ModuleInstance from one element of a module document's `instances` array.
         */
        static ModuleInstance fromDocument(const bsoncxx::document::view &doc);
    };

    /**
     * @brief Represents a module within the system: its static configuration plus the live pool
     * of instances the autoscaler is currently running for it.
     *
     * One Module document per module name - there is no longer a separate document per running
     * instance; instead each instance is an entry in `instances`, updated in place as it starts,
     * crashes, restarts or is scaled down.
     */
    struct Module {

        /**
         * @brief Unique identifier for this document.
         */
        std::string oid;

        /**
         * @brief Module name, e.g. "eam".
         */
        std::string name;

        /**
         * @brief Full path to the module's executable.
         */
        std::string executable;

        /**
         * @brief Full path to the module's configured (template) Unix domain socket, e.g.
         * "/var/run/euclid/euclid-eam.sock" - each instance's actual socket
         * (ModuleInstance::socketPath) is derived from this by inserting its pid.
         */
        std::string socketPath;

        /**
         * @brief Whether the module is enabled in configuration.
         */
        bool active = true;

        /**
         * @brief Whether this module is one of the installation's own, declared in euclid.json,
         * as opposed to a transfer server or an application pool the manager created at runtime.
         *
         * @par
         * The distinction matters because the runtime pools have their own desired state, held by
         * ETS and EAP and reconciled from there - stopping one through EMM would only be undone on
         * the next reconcile, so EMM refuses rather than pretending.
         */
        bool core = false;

        /**
         * @brief Whether somebody has stopped this module through EMM's stop-module.
         *
         * @par
         * Desired state rather than observed: the manager stops the module's instances when it
         * sees this and keeps them stopped - no restart, no autoscaling, no starting it again at
         * the next manager start - until start-module clears it. An instance that is merely
         * stopped, without this, is one the manager will bring straight back.
         */
        bool desiredStopped = false;

        /**
         * @brief Whether crashed instances of this module are automatically restarted.
         */
        bool autoRestart{};

        /**
         * @brief Maximum number of restarts allowed per instance (-1 = unlimited).
         */
        int maxRestarts = -1;

        /**
         * @brief Minimum number of autoscaled instances the controller keeps running for this module.
         */
        int minInstances = 1;

        /**
         * @brief Maximum number of autoscaled instances the controller may run for this module.
         */
        int maxInstances = 1;

        /**
         * @brief Instance limits asked for at runtime, or -1 for "nothing was asked".
         *
         * @par
         * Deliberately separate from minInstances/maxInstances above, which the manager rewrites
         * from its own configuration every time an instance changes state - a value written into
         * those by anything else survives only until the next crash, restart or scale event, which
         * is no way to hold a setting. These are written only by EMM's set-instances and read only
         * by the manager, so nothing overwrites them by accident.
         *
         * @par
         * They also outlive the manager: on start-up it applies them over what euclid.json says,
         * so a limit somebody set stays set until they change it again rather than reverting at
         * the next restart.
         */
        int desiredMinInstances = -1;

        /**
         * @brief See desiredMinInstances.
         */
        int desiredMaxInstances = -1;

        /**
         * @brief Worker threads asked for at runtime, or -1 for "nothing was asked".
         *
         * @par
         * Read by the module process itself as it starts (see
         * Core::HttpActionServer::ConfiguredWorkerThreads), taking precedence over
         * euclid.modules.<name>.threads in euclid.json - which is why, unlike the instance limits,
         * there is no corresponding field the manager writes back: the thread count is a property
         * of a running process rather than of the pool, and nothing but this ever records it.
         */
        int desiredThreads = -1;

        /**
         * @brief Level this module's own output is logged at by the manager, or empty to leave it
         * to the configuration.
         *
         * @par
         * A module writes to standard output and standard error; the manager reads that back and
         * logs it on the module's channel ("module.<name>"). This is that channel's level, kept
         * with the module rather than in a configuration file so it can be changed while
         * everything is running - see EMM's set-log-level action. "off" silences the module in the
         * manager's log entirely.
         *
         * @par
         * It decides what the manager passes on, not what the module produces: a line the module
         * wrote to standard output arrives here as information and one it wrote to standard error
         * as an error, whatever the module's own level said about it. Making a module say *more*
         * is euclid.logging.level in its own process - see Core::LogStream.
         */
        std::string logLevel;

        /**
         * @brief When somebody last asked for this module through EMM's restart-module, or the
         * epoch if nobody ever has.
         *
         * @par
         * A moment rather than a flag, because a restart is an event and not a state: the manager
         * remembers which one it has already carried out, so a second request restarts the module
         * a second time while re-reading the same one does nothing. Nothing clears it, and nothing
         * needs to - a request older than the manager is one its own start has already satisfied.
         */
        std::chrono::system_clock::time_point restartRequestedAt{};

        /**
         * @brief Process arguments list.
         */
        std::vector<std::string> args;

        /**
         * @brief Time this module document was first created.
         */
        std::chrono::system_clock::time_point created = std::chrono::system_clock::now();

        /**
         * @brief Time this module document was last updated.
         */
        std::chrono::system_clock::time_point modified = std::chrono::system_clock::now();

        /**
         * @brief Time this module's pool last went from having no running instances to having its
         * first one - the module's own "boot time", from which uptime is derived (e.g. in
         * euclid-ui). Default-constructed (epoch/1970) means the module has never had a running
         * instance. Stamped by the repository (see MongoEmmRepository::upsertInstance), not by
         * ServiceController, so it survives manager restarts.
         */
        std::chrono::system_clock::time_point lastStartTime{};

        /**
         * @brief Live pool of instances currently known for this module.
         */
        std::vector<ModuleInstance> instances;

        /**
         * @brief Converts the module's properties (including its instances) to a BSON document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Constructs a `Module` instance from a BSON document view.
         *
         * @param doc An optional BSON document view representing the data to construct the `Module`.
         *            If the document is not provided, an empty `Module` is returned.
         * @return A `Module` instance initialized with the data from the BSON document, or an empty
         *         `Module` instance if the document is absent.
         */
        [[maybe_unused]]
        static Module fromDocument(const std::optional<bsoncxx::document::view> &doc);
    };

    /**
     * @brief What a module's pool is doing, summed up from its instances.
     */
    struct PoolLoad {

        /**
         * @brief Instances in state RUNNING.
         */
        long running{};

        /**
         * @brief How many of those have said how loaded they are.
         */
        long reporting{};

        /**
         * @brief Mean utilisation across the reporting instances, 0 when none has reported.
         */
        double utilisation{};

        /**
         * @brief Total work waiting across the reporting instances.
         */
        long backlog{};
    };

    /**
     * @brief Summarises a pool for reporting.
     *
     * @par
     * The mean for utilisation and the total for backlog, because that is what each one means: half
     * a pool at 100% is a pool at 50%, while half a pool holding five hundred messages each is a
     * thousand messages waiting. Averaging the second, or adding the first, answers a question
     * nobody asked.
     *
     * @par
     * An instance that has never reported is counted in `running` and left out of both figures.
     * Treating it as zero would let a module that does not report at all read as an idle one.
     *
     * @param module the module record.
     * @return the summary; `reporting == 0` means only `running` is meaningful.
     */
    inline PoolLoad SummarisePool(const Module &module) {

        PoolLoad load;
        double utilisationSum = 0.0;

        for (const auto &instance: module.instances) {
            if (instance.state != ModuleState::RUNNING) continue;
            ++load.running;

            if (instance.utilisation < 0) continue;
            ++load.reporting;
            utilisationSum += instance.utilisation;
            if (instance.backlog > 0) load.backlog += instance.backlog;
        }

        if (load.reporting > 0) load.utilisation = utilisationSum / static_cast<double>(load.reporting);
        return load;
    }

}// namespace Euclid::Database::Entity::Module
