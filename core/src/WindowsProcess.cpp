// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#if defined(_WIN32)

// C++ includes
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <io.h>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/core/SystemUtils.h>
#include <euclid/core/UnixSocketServer.h>
#include <euclid/core/WindowsProcess.h>

namespace Euclid::Core::WindowsProcess {

    std::string ErrorText(const DWORD error) {
        char *text = nullptr;
        const auto length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                           nullptr, error, 0, reinterpret_cast<char *>(&text), 0, nullptr);

        std::string message = length > 0 && text ? std::string(text, length) : std::string();
        if (text) LocalFree(text);

        // FormatMessage ends its text with a newline, which would break the log line in two.
        while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back()))) message.pop_back();
        return message.empty() ? "error " + std::to_string(error) : message + " (error " + std::to_string(error) + ")";
    }

    namespace {

        /**
         * @brief The job object every process this one starts is put into, so that none of them
         * outlives it.
         *
         * @par
         * A starter that is killed - or that crashes, or is stopped the hard way - otherwise
         * leaves its children running: still consuming the queues their application listens to,
         * holding the HTTP port the next instance will be handed, reporting their load to nobody.
         * On a worker it is worse: the master re-places the work once the lease runs out, and a
         * child left running means the same slot twice. KILL_ON_JOB_CLOSE ends that - the job's
         * last handle closes when this process is torn down, however it is torn down, and the
         * kernel kills what is in it.
         *
         * @par
         * Created once and never closed: it is the process's own, and its lifetime is meant to be
         * exactly the process's. Nested jobs have been allowed since Windows 8, so a starter that
         * is itself inside somebody else's job - a service wrapper, a CI agent - still gets one.
         */
        HANDLE childJob() {
            static const HANDLE job = [] {
                const HANDLE created = CreateJobObjectA(nullptr, nullptr);
                if (!created) return static_cast<HANDLE>(nullptr);

                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                    CloseHandle(created);
                    return static_cast<HANDLE>(nullptr);
                }
                return created;
            }();
            return job;
        }

        // Restricts handle inheritance to exactly the two pipe write ends, so the child does not
        // also inherit every other inheritable handle this process happens to have open.
        struct AttributeList {
            std::vector<std::byte> buffer;
            LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

            bool init(HANDLE handles[], const DWORD count) {
                SIZE_T size = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                buffer.resize(size);
                list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
                if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
                    list = nullptr;
                    return false;
                }
                if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, count * sizeof(HANDLE), nullptr, nullptr)) {
                    DeleteProcThreadAttributeList(list);
                    list = nullptr;
                    return false;
                }
                return true;
            }

            ~AttributeList() {
                if (list) DeleteProcThreadAttributeList(list);
            }
        };

    }// namespace

    std::optional<Spawned> Spawn(const std::string &name, const std::vector<std::string> &commandLine,
                                 const std::map<std::string, std::string> &environment, const std::string &workingDir) {

        if (commandLine.empty()) {
            log_error << "Could not spawn " << name << ": no command";
            return std::nullopt;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
            log_error << "Could not create the output pipe for " << name << ": " << ErrorText(GetLastError());
            return std::nullopt;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
            log_error << "Could not create the error pipe for " << name << ": " << ErrorText(GetLastError());
            CloseHandle(outRead);
            CloseHandle(outWrite);
            return std::nullopt;
        }
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        // One flat string, each argument quoted the way the child's own argv parsing will undo -
        // so it arrives exactly as execvp() would have delivered it on POSIX.
        std::string cmdLine;
        for (const auto &argument: commandLine) {
            if (!cmdLine.empty()) cmdLine += ' ';
            cmdLine += SystemUtils::QuoteCommandLineArg(argument);
        }
        std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
        cmdLineBuf.push_back('\0');

        HANDLE inheritHandles[2] = {outWrite, errWrite};
        AttributeList attrs;
        const bool haveAttrs = attrs.init(inheritHandles, 2);

        STARTUPINFOEXA siex{};
        siex.StartupInfo.cb = sizeof(siex);
        siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        siex.StartupInfo.hStdOutput = outWrite;
        siex.StartupInfo.hStdError = errWrite;
        siex.StartupInfo.hStdInput = nullptr;
        siex.lpAttributeList = attrs.list;

        // Suspended, so the process is in the job before it executes an instruction: assigning it
        // afterwards is a race in which a child that spawns something of its own first leaves that
        // grandchild outside the job, which is the thing the job exists to stop.
        DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | CREATE_SUSPENDED;
        if (haveAttrs) flags |= EXTENDED_STARTUPINFO_PRESENT;

        // The child's SIGTERM - see Core::STOP_EVENT_VARIABLE for why a console control event
        // cannot be one. Manual-reset so it stays signalled once set: the child may be anywhere
        // between "still starting up" and "inside a request" when asked to stop, and an auto-reset
        // event would be consumed by whichever got to it first. Named, so the child opens it by
        // name and no handle has to cross the process boundary; unique among live children.
        static std::atomic<std::uint32_t> s_nextStopEventId{1};
        const std::string stopEventName = "Local\\euclid-stop-" + std::to_string(GetCurrentProcessId()) + "-" +
                                          std::to_string(s_nextStopEventId.fetch_add(1));
        const HANDLE stopEvent = CreateEventA(nullptr, TRUE, FALSE, stopEventName.c_str());

        // The whole environment as one NUL-separated, double-NUL-terminated block, this process's
        // own first: a block replaces the environment, so passing only the additions would start
        // the child with nothing else, not even PATH.
        std::map<std::string, std::string> merged;
        if (const char *existing = GetEnvironmentStringsA()) {
            for (const char *entry = existing; *entry != '\0'; entry += std::strlen(entry) + 1) {
                if (const std::string_view text(entry); text.find('=') != std::string_view::npos && !text.starts_with('=')) {
                    const auto split = text.find('=');
                    merged[std::string(text.substr(0, split))] = std::string(text.substr(split + 1));
                }
            }
            FreeEnvironmentStringsA(const_cast<char *>(existing));
        }
        for (const auto &[variable, value]: environment) merged[variable] = value;
        // Only when there is one to name. A child that finds it absent falls back to its console
        // handler, which is the right behaviour for a process nobody can signal.
        if (stopEvent) merged[STOP_EVENT_VARIABLE] = stopEventName;

        std::vector<char> environmentBlock;
        for (const auto &[variable, value]: merged) {
            const auto entry = variable + "=" + value;
            environmentBlock.insert(environmentBlock.end(), entry.begin(), entry.end());
            environmentBlock.push_back('\0');
        }
        environmentBlock.push_back('\0');

        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, flags, environmentBlock.data(),
                                       workingDir.empty() ? nullptr : workingDir.c_str(), &siex.StartupInfo, &pi);
        const auto createError = GetLastError();

        // The child (if created) holds its own copies of the write ends; this process's must be
        // closed, or the read ends never see EOF once the child exits.
        CloseHandle(outWrite);
        CloseHandle(errWrite);

        if (!ok) {
            // With everything needed to act on it: the whole command line, because a quoting or
            // path problem is only visible in the assembled thing, and the working directory,
            // because one that is not there is among the commonest reasons this fails.
            log_error << "Could not spawn " << name << ": " << ErrorText(createError) << ", command: " << cmdLine
                      << ", workingDir: " << (workingDir.empty() ? "(inherited)" : workingDir);
            CloseHandle(outRead);
            CloseHandle(errRead);
            if (stopEvent) CloseHandle(stopEvent);
            return std::nullopt;
        }

        // Into the job, then let it run. A failure here is not a failure to start: the process is
        // perfectly good, it is only not covered if its starter is killed - better than refusing
        // to run it at all.
        if (const HANDLE job = childJob(); job) {
            if (!AssignProcessToJobObject(job, pi.hProcess)) {
                log_warning << "Could not put " << name << " in this process's job object: " << ErrorText(GetLastError())
                            << " - it will outlive this process if this process is killed";
            }
        } else {
            log_warning << "No job object for spawned processes - " << name << " will outlive this process if it is killed";
        }

        const int stdoutFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY);
        const int stderrFd = _open_osfhandle(reinterpret_cast<intptr_t>(errRead), _O_RDONLY);
        if (stdoutFd == -1 || stderrFd == -1) {
            // Still suspended, so this kills a process that never ran an instruction - and says so,
            // because from the outside it would otherwise look like one that died on its own.
            log_error << "Could not take the output handles of " << name << ", killing pid " << pi.dwProcessId;
            if (stdoutFd == -1) CloseHandle(outRead);
            else _close(stdoutFd);
            if (stderrFd == -1) CloseHandle(errRead);
            else _close(stderrFd);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (stopEvent) CloseHandle(stopEvent);
            return std::nullopt;
        }

        // Everything is in place, so the child may run: nothing it writes on its first breath is
        // lost and nothing it spawns escapes the job.
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        return Spawned{.pid = pi.dwProcessId, .process = pi.hProcess, .stopEvent = stopEvent, .stdoutFd = stdoutFd, .stderrFd = stderrFd};
    }

    // ReSharper disable once CppParameterMayBeConst
    bool RequestStop(HANDLE stopEvent) {
        return stopEvent && SetEvent(stopEvent);
    }

    // ReSharper disable once CppParameterMayBeConst
    void Kill(HANDLE process) {
        if (process) TerminateProcess(process, 1);
    }

    // ReSharper disable once CppParameterMayBeConst
    bool IsRunning(HANDLE process) {
        return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    // ReSharper disable once CppParameterMayBeConst
    std::optional<DWORD> ExitCode(HANDLE process) {
        if (!process) return std::nullopt;
        // Asked of the wait rather than of STILL_ACTIVE alone: a process can exit with 259, which
        // GetExitCodeProcess cannot tell from "still running".
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return std::nullopt;
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process, &exitCode)) return std::nullopt;
        return exitCode;
    }

}// namespace Euclid::Core::WindowsProcess

#endif
