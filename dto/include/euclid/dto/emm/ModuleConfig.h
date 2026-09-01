//
// Created by jensv on 23/05/2026.
//

#pragma once

// C++ includes
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#ifdef _WIN32
#include <windows.h>
// See ModuleProcess.h for why this must be signed rather than DWORD.
using pid_t = std::int32_t;
#else
#include <sys/types.h>
#include <unistd.h>
#endif
#include <vector>

// Euclid includes
#include <euclid/database/entity/emm/ModuleState.h>

namespace Euclid::Dto {

    struct ModuleConfig {
        /**
         * @brief Module name
         */
        std::string name;

        /**
         * @brief Full path to executable
         */
        std::string executable;

        /**
         * @brief process arguments list
         */
        std::vector<std::string> args;

        /**
         * @brief Full path to UNIX domain socket
         */
        std::string socketPath;

        /**
         * @brief Restart delay
         */
        int restartDelayMs = 1000;

        /**
         * @brief Backoff cap
         */
        int maxRestartDelayMs = 30000;

        /**
         * @brief Maximal number of restarts (-1 = unlimited)
         */
        int maxRestarts = 5;

        /**
         * @brief Auto restart flag
         */
        bool autoRestart = true;

        /**
         * @brief Minimum number of instances to keep running (autoscaler floor)
         */
        int minInstances = 1;

        /**
         * @brief Maximum number of instances the autoscaler may spawn (autoscaler ceiling)
         */
        int maxInstances = 1;

        /**
         * @brief Environment variables set on the spawned process, on top of the manager's own.
         *
         * @par
         * Empty for the modules declared in euclid.json, which take everything they need from the
         * configuration file they are pointed at. It is applications that need this: a foreign
         * runtime cannot be told where its socket is through a euclid-specific command line
         * switch, and the credentials it signs its own calls with have no business on a command
         * line at all - see ServiceController::reconcileApplications().
         */
        std::map<std::string, std::string> environment;

        /**
         * @brief Directory the process is started in; empty leaves it inheriting the manager's.
         */
        std::string workingDir;

        /**
         * @brief How long an instance may take to create its socket before the manager gives up
         * on it and kills it, in milliseconds.
         *
         * @par
         * The euclid modules are listening within milliseconds, but a JVM or an interpreted
         * application can take seconds to get there, and killing one that was merely still
         * starting produces a restart loop rather than a slow start.
         */
        int readyTimeoutMs = 5000;
    };
}