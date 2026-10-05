// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#if defined(_WIN32)

// C++ includes
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace Euclid::Core {

    /**
     * @brief Starting, stopping and watching a child process on Windows - for whatever host starts
     * one.
     *
     * @par Why in core
     * Two things start processes on Windows: the manager, for euclid's modules and the applications
     * it runs itself, and euclid-wrk, for the applications placed on its node. The manager's
     * implementation took real care to get right - the job object, the inheritance list, starting
     * suspended, the stop event, the environment block - and a worker that wrote its own would have
     * been a second one to get right, and to drift. So it moved here, and both call it.
     * docs/worker-nodes.md §13.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace WindowsProcess {

        /**
         * @brief A process that was started, and what its starter holds on to it with.
         */
        struct Spawned {
            DWORD pid{};

            /**
             * @brief The process; the caller closes it once the process is gone.
             */
            HANDLE process{};

            /**
             * @brief This process's SIGTERM - see Core::STOP_EVENT_VARIABLE. Null when one could
             * not be created. The caller closes it once the process is gone.
             */
            HANDLE stopEvent{};

            /**
             * @brief The read ends of the process's stdout and stderr, as CRT file descriptors, so
             * the code that drains them reads with _read() and closes with _close() whatever the
             * pipe is underneath.
             */
            int stdoutFd{-1};
            int stderrFd{-1};
        };

        /**
         * @brief What Windows says went wrong, rather than only the number it says it with.
         */
        [[nodiscard]] std::string ErrorText(DWORD error);

        /**
         * @brief Starts @p commandLine with @p environment layered over this process's own, in
         * @p workingDir, its stdout and stderr redirected into two fresh pipes.
         *
         * @par
         *   - The child inherits exactly its two pipe write ends and no other handle this process
         *     has open - listening sockets, other children's pipes.
         *   - It is started suspended and put into a job object that is killed when this process
         *     goes, however it goes, and only then allowed to run - so neither it nor anything it
         *     starts can outlive its starter. A killed manager or worker leaves nothing behind
         *     still consuming queues, holding ports, or running a slot the master has already
         *     given to somebody else.
         *   - It is given a stop event, named in EUCLID_STOP_EVENT, which RequestStop() sets.
         *   - The environment is the whole of this process's plus @p environment, since a block
         *     passed to CreateProcess replaces the environment rather than adding to it.
         *
         * @param name what the process is, for the log lines a failure is reported with.
         * @param commandLine the executable, then its arguments; quoted here, one argument each.
         * @param environment added to, and overriding, this process's own.
         * @param workingDir where it starts; empty for this process's own.
         * @return what to hold the process by, or nothing - having said why - when it could not be
         * started. Nothing opened along the way is left open.
         */
        [[nodiscard]] std::optional<Spawned> Spawn(const std::string &name, const std::vector<std::string> &commandLine,
                                                   const std::map<std::string, std::string> &environment,
                                                   const std::string &workingDir);

        /**
         * @brief SIGTERM's counterpart: sets the process's stop event.
         *
         * @par
         * Not a console control event, which could never work from a service: it only reaches a
         * group sharing the caller's console, and a service has none. A process that does not
         * watch the event - an ordinary JVM - is not reached by this either, and is ended by
         * Kill() once its starter's stop timeout runs out.
         *
         * @return false when there is no event or it could not be set.
         */
        bool RequestStop(HANDLE stopEvent);

        /**
         * @brief SIGKILL's counterpart.
         */
        void Kill(HANDLE process);

        /**
         * @brief Whether the process has not yet exited.
         */
        [[nodiscard]] bool IsRunning(HANDLE process);

        /**
         * @brief How the process ended; nothing while it is still running or when it cannot be
         * asked.
         */
        [[nodiscard]] std::optional<DWORD> ExitCode(HANDLE process);

    }// namespace WindowsProcess

}// namespace Euclid::Core

#endif
