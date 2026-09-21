// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// ── POSIX (Linux + macOS) ─────────────────────────────────
#if defined(__linux__) || defined(__APPLE__)
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
  #include <csignal>
  #include <fcntl.h>
  #include <cerrno>
  #include <cstring>

  #ifdef __linux__
    #include <sys/mman.h>    // memfd_create, sendfile
    #include <sys/sendfile.h>
  #endif

// ── Windows ───────────────────────────────────────────────
#elif defined(_WIN32)
  #include <windows.h>
  #include <process.h>
  #include <io.h>
  #include <fcntl.h>
  // waitpid equivalent on Windows:
  // WaitForSingleObject(handle, timeoutMs)
  // GetExitCodeProcess(handle, &code)
#endif

// ── Always ────────────────────────────────────────────────
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <memory>

#if defined(_WIN32)

#include <euclid/dto/emm/ModuleConfig.h>

namespace Euclid::main::Platform {

    /**
     * @brief Reads from a pipe read-end fd, mirroring POSIX read() so drainPipe() in
     *        Controller.cpp stays identical on both platforms.
     */
    inline int PipeRead(const int fd, void *buf, const unsigned int n) { return _read(fd, buf, n); }

    /**
     * @brief Closes a pipe fd, mirroring POSIX close().
     */
    inline int PipeClose(const int fd) { return _close(fd); }

    /**
     * @brief Quotes a single argv element per the MSVC CommandLineToArgvW convention, so
     *        CreateProcess's flat command-line string round-trips through the child's own
     *        argv parsing exactly like execvp(argv[]) does on POSIX.
     */
    std::string QuoteArg(const std::string &arg);

    /**
     * @brief Spawns config.executable with the standard module argv (config.args + "--socket"
     *        instanceSocket), redirecting its stdout/stderr into two freshly-created pipes.
     *
     * The pipe read ends are returned as CRT file descriptors (via _open_osfhandle) so the
     * rest of Controller.cpp's pipe-draining code doesn't need to know it's a HANDLE
     * underneath.
     *
     * Also creates this instance's stop event and passes its name to the child in
     * EUCLID_STOP_EVENT - see Core::STOP_EVENT_VARIABLE. Ownership of that handle passes to
     * the caller, which must CloseHandle() it once the instance is gone; it is closed here
     * only if the spawn itself fails.
     *
     * @return false on failure; all handles/fds opened so far are closed before returning.
     */
    bool SpawnInstance(const Dto::ModuleConfig &config, const std::string &instanceSocket,
                        pid_t &outPid, HANDLE &outProcessHandle, HANDLE &outStopEvent,
                        int &outStdoutFd, int &outStderrFd);

    /**
     * @brief SIGTERM-equivalent: sets the instance's stop event, which is what
     *        UnixSocketServer::RunUntilSignal() is waiting on.
     *
     * @par
     * This used to send CTRL_BREAK_EVENT to the process group instead, which could never work
     * from a process running as a Windows service: GenerateConsoleCtrlEvent() only reaches a
     * group sharing the caller's console, and a service has none. The call failed silently and
     * every module was TerminateProcess()d after its stop timeout - see STOP_EVENT_VARIABLE.
     *
     * @return false if the instance has no stop event, or the event could not be set - in
     *         which case nothing was asked of the process and the caller's timeout will
     *         expire in full before it escalates to ForceKill().
     */
    bool RequestGracefulStop(HANDLE stopEvent);

    /**
     * @brief SIGKILL-equivalent.
     */
    void ForceKill(HANDLE processHandle);

    /**
     * @brief True if the process has not yet exited.
     */
    bool IsRunning(HANDLE processHandle);

    /**
     * @brief Registers the currently running executable as the Windows service "euclid"
     *        (SERVICE_AUTO_START, running as Local System), with `--config configFilePath`
     *        as its startup command line. Mirrors the installer's own
     *        SimpleSC::InstallService call (dist/win32/msi/product.nsi), for anyone who
     *        deploys the binaries without running the NSIS installer.
     *
     * @return empty string on success; a human-readable error message otherwise (e.g.
     *         "already exists", "access denied - run as Administrator").
     */
    std::string InstallService(const std::string &configFilePath);

}// namespace Euclid::main::Platform

#endif
