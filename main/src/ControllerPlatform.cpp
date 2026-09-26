// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#if defined(_WIN32)

// C++ includes
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/core/UnixSocketServer.h>
#include <euclid/manager/ControllerPlatform.h>

namespace Euclid::main::Platform {

    namespace {
        // What Windows says went wrong, rather than only the number it says it with. A spawn
        // failure is read by whoever is wondering why a pool has no processes, and "error 267"
        // sends them to a search engine where "The directory name is invalid" ends it.
        std::string LastError(const DWORD error) {
            char *text = nullptr;
            const auto length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                                       | FORMAT_MESSAGE_IGNORE_INSERTS,
                                               nullptr, error, 0, reinterpret_cast<char *>(&text), 0, nullptr);

            std::string message = length > 0 && text ? std::string(text, length) : std::string();
            if (text) LocalFree(text);

            // FormatMessage ends its text with a newline, which would break the log line in two.
            while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back()))) {
                message.pop_back();
            }
            return message.empty() ? "error " + std::to_string(error)
                                   : message + " (error " + std::to_string(error) + ")";
        }
    }// namespace

    // Standard MSVC/CommandLineToArgvW quoting algorithm: only quotes when needed, and
    // doubles up backslashes that immediately precede either a literal quote or the
    // closing quote, so the child's own argv parser recovers exactly the original string.
    std::string QuoteArg(const std::string &arg) {
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
            return arg;
        }

        std::string result = "\"";
        for (auto it = arg.begin();; ++it) {
            unsigned backslashes = 0;
            while (it != arg.end() && *it == '\\') {
                ++it;
                ++backslashes;
            }

            if (it == arg.end()) {
                result.append(backslashes * 2, '\\');
                break;
            }
            if (*it == '"') {
                result.append(backslashes * 2 + 1, '\\');
                result.push_back('"');
            } else {
                result.append(backslashes, '\\');
                result.push_back(*it);
            }
        }
        result.push_back('"');
        return result;
    }

    namespace {

        /**
         * @brief The job object every instance this manager spawns is put into, so that none of
         * them outlives it.
         *
         * @par
         * A manager that is killed - or that crashes, or is stopped the hard way - leaves its
         * children running: they go on consuming the queues their application listens to, holding
         * the HTTP port the next instance will be handed, and reporting their load to a manager
         * that has no record of them. What that looks like afterwards is messages handled twice,
         * an application that cannot bind its port, and "load report matched no instance" every
         * fifteen seconds from a process nobody can account for. Seen on this installation with
         * four dead managers' worth of leftovers at once.
         *
         * @par
         * KILL_ON_JOB_CLOSE ends that: the job's last handle closes when the manager's process
         * object is torn down, however it is torn down, and the kernel kills what is in it. There
         * is nothing to reap afterwards because nothing is left.
         *
         * @par
         * The cost, stated plainly: an application cannot outlive the manager on purpose either.
         * A restart stops every instance and starts it again, which is what a restart already did
         * on the shutdown path - this only makes the crash path behave the same way.
         *
         * @par
         * Created once and never closed: it is the process's own, and its lifetime is meant to be
         * exactly the process's. Nested jobs have been allowed since Windows 8, so a manager that
         * is itself inside somebody else's job - a service wrapper, a CI agent - still gets one.
         */
        HANDLE instanceJob() {
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

        // Restricts handle inheritance to exactly the two pipe write-ends, so the child
        // doesn't also inherit every other inheritable handle the manager happens to have
        // open (listening sockets, other modules' pipes, ...) - the default
        // bInheritHandles=TRUE behavior without this attribute list.
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
                if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                handles, count * sizeof(HANDLE), nullptr, nullptr)) {
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

    bool SpawnInstance(const Dto::ModuleConfig &config, const std::string &instanceSocket,
                        pid_t &outPid, HANDLE &outProcessHandle, HANDLE &outStopEvent,
                        int &outStdoutFd, int &outStderrFd) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
            log_error << "Could not create the output pipe for " << config.name << ": " << LastError(GetLastError());
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
            CloseHandle(outRead);
            CloseHandle(outWrite);
            return false;
        }
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        std::string cmdLine = QuoteArg(config.executable);
        for (auto &a: config.args) {
            cmdLine += ' ';
            cmdLine += QuoteArg(a);
        }
        cmdLine += " --socket ";
        cmdLine += QuoteArg(instanceSocket);

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

        PROCESS_INFORMATION pi{};
        // Suspended, so the process is in the job below before it executes an instruction:
        // assigning it afterwards is a race in which a child that spawns something of its own
        // first leaves that grandchild outside the job, which is the thing the job exists to stop.
        DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | CREATE_SUSPENDED;
        if (haveAttrs) flags |= EXTENDED_STARTUPINFO_PRESENT;

        // This instance's SIGTERM - see Core::STOP_EVENT_VARIABLE for why a console control
        // event cannot be one. Manual-reset so it stays signalled once set: the child may be
        // anywhere between "still starting up" and "already inside a request" when the manager
        // asks it to stop, and an auto-reset event would be consumed by whichever of those got
        // to it first. Named rather than inherited, so the child can open it by name without
        // the manager having to hand a handle across the process boundary.
        //
        // The name only has to be unique among live instances and agreed on by both sides. It
        // is built rather than derived from instanceSocket because that is a path, and the
        // backslashes and colon in a Windows one are not legal in a kernel object name.
        static std::atomic<std::uint32_t> s_nextStopEventId{1};
        const std::string stopEventName = "Local\\euclid-stop-" + std::to_string(GetCurrentProcessId())
                                        + "-" + std::to_string(s_nextStopEventId.fetch_add(1));
        const HANDLE stopEvent = CreateEventA(nullptr, TRUE, FALSE, stopEventName.c_str());

        // An application's environment (and the socket path, which a foreign runtime reads from
        // EUCLID_SOCKET rather than off the command line) has to be materialised as one
        // NUL-separated, double-NUL-terminated block, on top of whatever the manager itself has -
        // passing only the additions would start the child with nothing else, not even PATH.
        //
        // Built unconditionally, where it used to be skipped for a module that added nothing of
        // its own: EUCLID_STOP_EVENT is added to every instance, so there is no longer such a
        // thing as a child that needs no additions. Merging over GetEnvironmentStringsA() first
        // means a child that adds nothing still gets exactly the environment it inherited before.
        std::map<std::string, std::string> merged;
        if (const char *existing = GetEnvironmentStringsA()) {
            for (const char *entry = existing; *entry != '\0'; entry += std::strlen(entry) + 1) {
                if (const std::string_view text(entry); text.find('=') != std::string_view::npos && !text.starts_with('=')) {
                    const auto split = text.find('=');
                    merged[std::string(text.substr(0, split))] = std::string(text.substr(split + 1));
                }
            }
        }
        for (const auto &[name, value]: config.environment) merged[name] = value;
        merged["EUCLID_SOCKET"] = instanceSocket;
        // Only when there is one to name. A child that finds the variable absent falls back to
        // its console handler, which is the right behaviour for a process nobody can signal.
        if (stopEvent) merged[Core::STOP_EVENT_VARIABLE] = stopEventName;

        std::vector<char> environmentBlock;
        for (const auto &[name, value]: merged) {
            const auto entry = name + "=" + value;
            environmentBlock.insert(environmentBlock.end(), entry.begin(), entry.end());
            environmentBlock.push_back('\0');
        }
        environmentBlock.push_back('\0');

        const BOOL ok = CreateProcessA(
                nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, flags,
                environmentBlock.data(),
                config.workingDir.empty() ? nullptr : config.workingDir.c_str(), &siex.StartupInfo, &pi);
        const auto createError = GetLastError();

        // The child (if created) now holds its own copies of the write ends; the parent's
        // copies must be closed or the pipe's read end never sees EOF once the child exits.
        CloseHandle(outWrite);
        CloseHandle(errWrite);

        if (!ok) {
            // Said out loud, and with everything needed to act on it: the whole command line,
            // because a quoting or path problem is only visible in the assembled thing, and the
            // working directory, because a directory that is not there is one of the two commonest
            // reasons this fails - the other being an executable that is not where the
            // configuration says.
            //
            // This used to return false without a word, and the caller dropped the answer, so an
            // application that could not be spawned produced no output at any level: no process, no
            // instance record, and a log that said nothing at all. That is a worse failure than the
            // one it was hiding.
            log_error << "Could not spawn " << config.name << ": " << LastError(createError)
                      << ", command: " << cmdLine
                      << ", workingDir: " << (config.workingDir.empty() ? "(inherited)" : config.workingDir);

            CloseHandle(outRead);
            CloseHandle(errRead);
            if (stopEvent) CloseHandle(stopEvent);
            return false;
        }
        // Into the job that dies with this process, then let it run. A failure here is not a
        // failure to start: the instance is perfectly good, it simply is not covered if the
        // manager is killed - which is what the installation looked like before the job existed,
        // and is better than refusing to run the application at all.
        if (const HANDLE job = instanceJob(); job) {
            if (!AssignProcessToJobObject(job, pi.hProcess)) {
                log_warning << "Could not put " << config.name << " in the manager's job object: " << LastError(GetLastError())
                            << " - it will outlive the manager if the manager is killed";
            }
        } else {
            log_warning << "No job object for spawned instances - an instance will outlive a manager that is killed";
        }

        const int stdoutFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY);
        const int stderrFd = _open_osfhandle(reinterpret_cast<intptr_t>(errRead), _O_RDONLY);
        if (stdoutFd == -1 || stderrFd == -1) {
            // Still suspended, so this kills a process that never ran an instruction - and says so,
            // because from the outside it would otherwise look like one that died on its own.
            log_error << "Could not take the output handles of " << config.name << ", killing pid " << pi.dwProcessId;
            if (stdoutFd == -1) CloseHandle(outRead); else _close(stdoutFd);
            if (stderrFd == -1) CloseHandle(errRead); else _close(stderrFd);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (stopEvent) CloseHandle(stopEvent);
            return false;
        }

        // Everything the parent needs is in place, so the child may run: the job holds it and the
        // pipes are wrapped, so nothing it writes on its first breath is lost and nothing it
        // spawns escapes the job.
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        outPid = static_cast<pid_t>(pi.dwProcessId);
        outProcessHandle = pi.hProcess;
        outStopEvent = stopEvent;
        outStdoutFd = stdoutFd;
        outStderrFd = stderrFd;
        return true;
    }

    // ReSharper disable once CppParameterMayBeConst
    bool RequestGracefulStop(HANDLE stopEvent) {
        return stopEvent && SetEvent(stopEvent);
    }

    // ReSharper disable once CppParameterMayBeConst
    void ForceKill(HANDLE processHandle) {
        if (processHandle) TerminateProcess(processHandle, 1);
    }

    // ReSharper disable once CppParameterMayBeConst
    bool IsRunning(HANDLE processHandle) {
        if (!processHandle) return false;
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(processHandle, &exitCode)) return false;
        return exitCode == STILL_ACTIVE;
    }

    std::string InstallService(const std::string &configFilePath) {
        char exePath[MAX_PATH];
        const DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len == 0 || len == MAX_PATH) return "Failed to resolve the running executable's own path";

        const std::string binPath = QuoteArg(exePath) + " --config " + QuoteArg(configFilePath);

        const SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm) {
            if (GetLastError() == ERROR_ACCESS_DENIED) return "Access denied - re-run as Administrator";
            return "Failed to open the Service Control Manager (error " + std::to_string(GetLastError()) + ")";
        }

        const SC_HANDLE svc = CreateServiceA(
                scm, "euclid", "Euclid",
                SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

        if (!svc) {
            const DWORD err = GetLastError();
            CloseServiceHandle(scm);
            if (err == ERROR_SERVICE_EXISTS) return "Service 'euclid' is already installed";
            if (err == ERROR_ACCESS_DENIED) return "Access denied - re-run as Administrator";
            return "CreateService failed (error " + std::to_string(err) + ")";
        }

        SERVICE_DESCRIPTIONA desc{const_cast<char *>("Euclid Cloud Services")};
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return "";
    }

}// namespace Euclid::main::Platform

#endif
