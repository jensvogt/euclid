//
// Created by jensv on 23/05/2026.
//

#pragma once

// C++ includes
#include <chrono>
#include <cstdint>
#include <string>
#ifdef _WIN32
#include <windows.h>
// Signed so the existing -1 "no process" sentinel and pid>0/pid<=0 checks
// throughout ServiceController work as-is; DWORD (unsigned) would make -1
// wrap to a huge positive value and break those comparisons. Real Windows
// PIDs fit comfortably within int32_t.
using pid_t = std::int32_t;
#else
#include <sys/types.h>
#include <unistd.h>
#endif
#include <vector>

// Euclid includes
#include <euclid/core/UuidUtils.h>
#include <euclid/dto/emm/ModuleConfig.h>
#include <euclid/database/entity/emm/ModuleState.h>

namespace Euclid::Dto {

    struct ModuleProcess {

        /**
         * @brief Module config
         */
        ModuleConfig config;

        /**
         * @brief Stable identifier for this pool slot, generated once when the slot is created
         * and unchanged across restarts (unlike pid, which changes every time the process is
         * respawned). Used as the key to update this slot's entry in the database's per-module
         * instances array in place, instead of appending a new entry on every restart.
         */
        std::string instanceId = Core::UuidUtils::CreateRandomUuid();

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

#ifdef _WIN32
        /**
         * @brief Windows process handle from CreateProcess(), used for TerminateProcess()/
         *        GetExitCodeProcess() since Windows has no waitpid()-by-pid equivalent.
         */
        HANDLE processHandle = nullptr;
#endif

        /**
         * @brief Actual (PID-suffixed) Unix domain socket path this running instance is bound to.
         *
         * Derived from config.socketPath by inserting the instance's pid, e.g.
         * "/var/run/euclid/sqs.sock" -> "/var/run/euclid/sqs.4242.sock". Empty while stopped.
         */
        std::string instanceSocketPath;

        /**
         * @brief TCP port handed to this instance for its own HTTP listener, or 0 if the module
         * was not given a port range to draw from. Passed to the process as EUCLID_HTTP_PORT.
         */
        int httpPort{};

        /**
         * @brief Number of requests currently being forwarded to this instance.
         *
         * Incremented/decremented by ServiceController::acquireInstance/releaseInstance under
         * ServiceController::_mutex; drives the autoscaler's scale up/down decisions.
         */
        int activeRequests = 0;

        /**
         * @brief Requests currently being served by this instance, whether or not they count as
         * load.
         *
         * @par
         * Deliberately separate from activeRequests. A long poll is not load - it is a caller
         * waiting for something to arrive, and counting it makes an idle pool look saturated - but
         * it is very much still in flight, and stopping the instance underneath it kills a live
         * connection. Answering "is this instance busy" and "is this instance safe to stop" with
         * one number is what made a scale-down report "end of stream" to whoever was waiting.
         */
        int inFlightRequests{};

        /**
         * @brief Time this instance's activeRequests last dropped to zero.
         *
         * Used by the autoscaler to determine how long an instance has been idle before
         * scaling it down.
         */
        std::chrono::steady_clock::time_point lastIdleAt = std::chrono::steady_clock::now();

        /**
         * @brief True if acquireInstance() picked this instance at any point since the
         * autoscaler's last evaluateScaling() check.
         *
         * A single forwarded request typically holds activeRequests > 0 for only as long as the
         * gateway-to-module hop over a local socket takes - often just milliseconds - which is far
         * shorter than the watchdog's 1-second sampling interval. Checking activeRequests alone at
         * the sampling instant means an instance's brief busy windows are very likely to fall
         * *between* two ticks, so the autoscaler undercounts real demand and scale-up stalls well
         * below what the traffic actually warrants. This flag is set on every acquireInstance()
         * pick and cleared once per tick after evaluateScaling() reads it, so any burst that
         * happened anywhere in the last ~1s still counts as busy, not just one caught red-handed.
         */
        bool wasBusySinceLastCheck = false;
    };
}