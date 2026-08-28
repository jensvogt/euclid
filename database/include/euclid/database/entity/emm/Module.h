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
         * @brief Whether crashed instances of this module are automatically restarted.
         */
        bool autoRestart{};

        /**
         * @brief Maximum number of restarts allowed per instance (-1 = unlimited).
         */
        int maxRestarts = -1;

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
