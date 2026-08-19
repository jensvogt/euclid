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
#include <euclid/dto/module/ModuleConfig.h>
#include <euclid/database/entity/module/ModuleState.h>

namespace Euclid::Dto {

    struct ModuleProcess {

        /**
         * @brief Module config
         */
        ModuleConfig config;

        /**
         * @brief Process ID
         */
        pid_t pid = -1;

        /**
         * @brief Module state
         */
        Database::Entity::ModuleState state = Database::Entity::ModuleState::STOPPED;

        /**
         * @brief Restart count
         */
        int restartCount = 0;

        /**
         * @brief Current restart delay
         */
        int currentDelay = 1000;

        /**
         * @brief Process start time
         */
        std::chrono::steady_clock::time_point startTime;

        /**
         * @brief Time of last crash
         */
        std::chrono::steady_clock::time_point lastCrashTime;

        /**
         * @brief stdout capture
         */
        int stdoutFd = -1;

        /**
         * @brief stderr capture
         */
        int stderrFd = -1;

        /**
         * @brief Actual (PID-suffixed) Unix domain socket path this running instance is bound to.
         *
         * Derived from config.socketPath by inserting the instance's pid, e.g.
         * "/var/run/euclid/sqs.sock" -> "/var/run/euclid/sqs.4242.sock". Empty while stopped.
         */
        std::string instanceSocketPath;

        /**
         * @brief Number of requests currently being forwarded to this instance.
         *
         * Incremented/decremented by ServiceController::acquireInstance/releaseInstance under
         * ServiceController::_mutex; drives the autoscaler's scale up/down decisions.
         */
        int activeRequests = 0;

        /**
         * @brief Time this instance's activeRequests last dropped to zero.
         *
         * Used by the autoscaler to determine how long an instance has been idle before
         * scaling it down.
         */
        std::chrono::steady_clock::time_point lastIdleAt = std::chrono::steady_clock::now();
    };
}