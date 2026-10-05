// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#if defined(_WIN32)

// C++ includes
#include <map>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/core/SystemUtils.h>
#include <euclid/core/WindowsProcess.h>
#include <euclid/manager/ControllerPlatform.h>

namespace Euclid::main::Platform {

    // Core::SystemUtils::QuoteCommandLineArg() under its original name. The rule moved to core
    // when euclid-wrk came to need it too - it spawns the applications placed on its host, and
    // links none of this. Two copies of the backslash-doubling rule is how two copies come to
    // disagree about a path with a space in it.
    std::string QuoteArg(const std::string &arg) {
        return Core::SystemUtils::QuoteCommandLineArg(arg);
    }

    // The process itself is started by Core::WindowsProcess, which euclid-wrk starts the
    // applications placed on its node with too - the job object, the inheritance list, the
    // suspended start and the stop event are one implementation. What is the manager's is the
    // shape of a module's command line: its arguments, then the socket it is to listen on, under
    // --socket and as EUCLID_SOCKET for a runtime that reads its environment rather than its argv.
    bool SpawnInstance(const Dto::ModuleConfig &config, const std::string &instanceSocket,
                       pid_t &outPid, HANDLE &outProcessHandle, HANDLE &outStopEvent,
                       int &outStdoutFd, int &outStderrFd) {

        std::vector<std::string> commandLine{config.executable};
        commandLine.insert(commandLine.end(), config.args.begin(), config.args.end());
        commandLine.emplace_back("--socket");
        commandLine.push_back(instanceSocket);

        auto environment = config.environment;
        environment["EUCLID_SOCKET"] = instanceSocket;

        const auto spawned = Core::WindowsProcess::Spawn(config.name, commandLine, environment, config.workingDir);
        if (!spawned.has_value()) return false;

        outPid = static_cast<pid_t>(spawned->pid);
        outProcessHandle = spawned->process;
        outStopEvent = spawned->stopEvent;
        outStdoutFd = spawned->stdoutFd;
        outStderrFd = spawned->stderrFd;
        return true;
    }

    // ReSharper disable once CppParameterMayBeConst
    bool RequestGracefulStop(HANDLE stopEvent) {
        return Core::WindowsProcess::RequestStop(stopEvent);
    }

    // ReSharper disable once CppParameterMayBeConst
    void ForceKill(HANDLE processHandle) {
        Core::WindowsProcess::Kill(processHandle);
    }

    // ReSharper disable once CppParameterMayBeConst
    bool IsRunning(HANDLE processHandle) {
        return Core::WindowsProcess::IsRunning(processHandle);
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
                scm, "euclid", "Euclid Cloud Service",
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
