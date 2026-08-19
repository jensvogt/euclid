//
// Created by jensv on 23/05/2026.
//

#pragma once

// C++ includes
#include <chrono>
#include <string>
#ifdef _WIN32
#include <windows.h>
using pid_t = DWORD;
#else
#include <sys/types.h>
#include <unistd.h>
#endif
#include <vector>

// Euclid includes
#include <euclid/database/entity/module/ModuleState.h>

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
    };
}