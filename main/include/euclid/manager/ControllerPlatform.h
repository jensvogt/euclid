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

#include <euclid/dto/module/ModuleConfig.h>

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
     * underneath. Created with CREATE_NEW_PROCESS_GROUP so RequestGracefulStop() can later
     * target this process (and only this process) with CTRL_BREAK_EVENT.
     *
     * @return false on failure; all handles/fds opened so far are closed before returning.
     */
    bool SpawnInstance(const Dto::ModuleConfig &config, const std::string &instanceSocket,
                        pid_t &outPid, HANDLE &outProcessHandle, int &outStdoutFd, int &outStderrFd);

    /**
     * @brief SIGTERM-equivalent: sends CTRL_BREAK_EVENT to the process's group.
     *
     * Best-effort - only triggers graceful shutdown if the target installed a console
     * control handler (see UnixSocketServer::RunUntilSignal's Windows branch). Processes
     * that didn't are simply terminated by the OS's default handling, which is an
     * acceptable fallback given the caller is already tearing the instance down and will
     * escalate to ForceKill() if it doesn't exit in time regardless.
     */
    void RequestGracefulStop(pid_t pid);

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
