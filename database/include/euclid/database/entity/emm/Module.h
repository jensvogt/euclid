//
// Created by vogje01 on 10/22/23.
//

#pragma once

// C++ includes
#include <string>
#include <chrono>

// Mongo Db includes
#include <bsoncxx/builder/basic/document.hpp>

// Euclid includes
#include <euclid/database/entity/emm/ModuleState.h>

namespace Euclid::Database::Entity {

    /**
     * @brief Represents a module within the system, encapsulating its metadata, state,
     * and operational properties.
     *
     * The `Module` structure provides a clearly defined
     * abstraction for managing individual modules, their configurations, and runtime
     * behaviors.
     */
    struct Module {

        /**
         * @brief Represents the unique identifier for an object within the system.
         *
         * This variable is used to uniquely distinguish objects across various
         * modules or components, ensuring proper identification and tracking.
         */
        std::string oid;

        /**
         * @brief Stores the name or identifier as a string value.
         *
         * The `name` variable is used to represent a textual label or
         * description, providing a means to uniquely or descriptively
         * identify an entity within the context of the system.
         */
        std::string name;

        /**
         * @brief Represents the path or name of an executable associated with a module.
         *
         * The `executable` variable is used to define or reference the executable
         * file that a module relies on to perform its operations or tasks during runtime.
         */
        std::string executable;

        /**
         * @brief Indicates whether the module is currently active or enabled.
         *
         * This variable is used to represent the operational state of the module,
         * determining if it is actively functioning or has been disabled. When set to `true`,
         * the module is active and operational; when set to `false`, the module is inactive
         * or disabled. This flag plays a critical role in managing and controlling the
         * execution flow or availability of the module within the system.
         */
        bool active = true;

        /**
         * @brief Represents the current status or condition of the system, module, or component.
         *
         * This variable is used to store the operational state, which may indicate
         * various predefined conditions or modes of operation. It plays a central role
         * in determining the behavior and response of the system based on its value.
         */
        ModuleState state = ModuleState::STOPPED;

        /**
         * @brief Specifies the filesystem path to a Unix domain socket that is used for
         * interprocess communication with the module.
         *
         * This variable holds the path to the socket file, which enables communication
         * between the module and its associated processes or services. The socket
         * path is typically used for transmitting control commands or exchanging
         * data between the module and other components. It must point to a valid,
         * writable location on the filesystem.
         *
         * Key Points:
         * - Used in serialization and deserialization to/from BSON documents.
         * - Important for configuring and managing interprocess communication.
         */
        std::string socketPath;

        /**
         * @brief PID of the process
         */
        int pid = -1;

        /**
         * @brief Tracks the number of times the module process has been restarted.
         *
         * This variable is incremented each time the module is restarted, either
         * automatically or manually, and is used to monitor the cumulative number
         * of restarts for the current lifecycle of the module.
         *
         * It is primarily intended for diagnostic, monitoring, or operational
         * purposes, such as managing restart policies or identifying problematic
         * module behavior due to repeated restarts.
         *
         * The restart count may be reset under certain conditions, such as when the
         * module configuration is reloaded or the module instance is reinitialized.
         */
        int restartCount{};

        /**
         * @brief A flag indicating whether the module should automatically restart upon failure.
         *
         * When set to `true`, the module is configured to restart automatically if it stops or crashes.
         * This flag can help ensure the module remains operational in the event of unexpected failures.
         * If set to `false`, the module will not automatically restart and must be manually restarted if needed.
         */
        bool autoRestart{};

        /**
         * @brief Specifies the maximum number of times a module process is allowed to be restarted.
         *
         * A negative value indicates no limit on the number of restarts.
         * This variable is typically used in conjunction with the autoRestart property
         * to determine if a module can be restarted when it stops unexpectedly.
         */
        int maxRestarts = -1;

        /**
         * @brief Represents a collection of variadic arguments passed to a function or operation.
         *
         * The `args` variable serves as a container for zero or more arguments,
         * providing flexibility in defining functions or operations that can handle
         * an arbitrary number of inputs.
         */
        std::vector<std::string> args;

        /**
         * @brief Stores the timestamp representing the creation time point.
         *
         * This variable holds the precise time of creation using the system clock,
         * providing a reliable reference for operations requiring temporal context.
         */
        std::chrono::system_clock::time_point created = std::chrono::system_clock::now();

        /**
         * @brief Indicates whether the configuration or state of the system has been altered.
         *
         * This variable is set to true whenever a modification occurs, signaling that
         * changes have been made which may require further action, such as saving or
         * processing updates. When no changes are present, it remains false.
         */
        std::chrono::system_clock::time_point modified = std::chrono::system_clock::now();

        /**
         * @brief Converts the module's properties to a BSON document representation.
         *
         * This method creates a BSON document containing key-value pairs that represent
         * the module's configuration and state. Each field in the module is serialized to
         * a corresponding key in the BSON document.
         *
         * @return A BSON document containing the module's properties, such as name,
         *         executable path, socket path, state, process ID, restart-related configurations,
         *         and activity status.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Constructs a `Module` instance from a BSON document view.
         *
         * The method extracts relevant fields from the BSON document and
         * populates the corresponding properties of the `Module` instance.
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