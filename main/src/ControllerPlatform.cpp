// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#if defined(_WIN32)

// C++ includes
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Euclid includes
#include <euclid/core/UnixSocketServer.h>
#include <euclid/manager/ControllerPlatform.h>

namespace Euclid::main::Platform {

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
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return false;
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
        DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
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

        // The child (if created) now holds its own copies of the write ends; the parent's
        // copies must be closed or the pipe's read end never sees EOF once the child exits.
        CloseHandle(outWrite);
        CloseHandle(errWrite);

        if (!ok) {
            CloseHandle(outRead);
            CloseHandle(errRead);
            if (stopEvent) CloseHandle(stopEvent);
            return false;
        }
        CloseHandle(pi.hThread);

        const int stdoutFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY);
        const int stderrFd = _open_osfhandle(reinterpret_cast<intptr_t>(errRead), _O_RDONLY);
        if (stdoutFd == -1 || stderrFd == -1) {
            if (stdoutFd == -1) CloseHandle(outRead); else _close(stdoutFd);
            if (stderrFd == -1) CloseHandle(errRead); else _close(stderrFd);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            if (stopEvent) CloseHandle(stopEvent);
            return false;
        }

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
