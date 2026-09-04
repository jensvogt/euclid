//
// Created by vogje01 on 10/22/23.
//

#pragma once

// C++ includes
#include <string>
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
         */
        int pid = -1;

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

}// namespace Euclid::Database::Entity::Module
