// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <WorkerService.h>

#if defined(_WIN32)

// Windows includes
#include <windows.h>

// C++ includes
#include <chrono>
#include <thread>

// Euclid includes
#include <euclid/core/SystemUtils.h>

namespace Euclid::Worker::Service {

    namespace {

        constexpr auto DisplayName = "Euclid Worker Node";
        constexpr auto Description = "Runs the applications euclid places on this host.";

        std::string LastErrorText(const char *what, const DWORD error) {
            if (error == ERROR_ACCESS_DENIED) return "Access denied - re-run as Administrator";
            return std::string(what) + " failed (error " + std::to_string(error) + ")";
        }

    }// namespace

    std::string Install(const std::string &configFilePath, const std::string &credentialsFilePath) {

        char exePath[MAX_PATH];
        const DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len == 0 || len == MAX_PATH) return "Failed to resolve the running executable's own path";

        // Quoted through core rather than by hand: the service's binPath is read back by
        // CommandLineToArgvW in this same executable's main(), so "C:\Program Files\..." has to
        // survive the round trip. See SystemUtils::QuoteCommandLineArg for why that is not just a
        // matter of wrapping it in quotes.
        std::string binPath = Core::SystemUtils::QuoteCommandLineArg(exePath);
        if (!configFilePath.empty()) binPath += " --config " + Core::SystemUtils::QuoteCommandLineArg(configFilePath);
        if (!credentialsFilePath.empty()) binPath += " --credentials " + Core::SystemUtils::QuoteCommandLineArg(credentialsFilePath);

        const SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm) return LastErrorText("Opening the Service Control Manager", GetLastError());

        // SERVICE_AUTO_START, and deliberately not started here. This matches what the Debian
        // package does - "systemctl enable" and no "systemctl start" - and for the same reason: a
        // worker with no credentials exits 1, so starting it now would report a failure for a state
        // that is simply "not configured yet". It comes up on the next boot, or when an operator
        // starts it after putting the credentials file in place.
        const SC_HANDLE svc = CreateServiceA(
                scm, Name, DisplayName,
                SERVICE_CHANGE_CONFIG, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

        if (!svc) {
            const DWORD error = GetLastError();
            CloseServiceHandle(scm);
            if (error == ERROR_SERVICE_EXISTS) return std::string("Service '") + Name + "' is already installed";
            return LastErrorText("CreateService", error);
        }

        SERVICE_DESCRIPTIONA description{const_cast<char *>(Description)};
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &description);

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return "";
    }

    std::string Uninstall() {

        const SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return LastErrorText("Opening the Service Control Manager", GetLastError());

        const SC_HANDLE svc = OpenServiceA(scm, Name, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
        if (!svc) {
            const DWORD error = GetLastError();
            CloseServiceHandle(scm);
            if (error == ERROR_SERVICE_DOES_NOT_EXIST) return std::string("Service '") + Name + "' is not installed";
            return LastErrorText("OpenService", error);
        }

        // Stopped before it is deleted - see the header. The stop is asked for and waited on rather
        // than assumed: DeleteService on a running service succeeds and does nothing visible, and
        // the next --install then refuses because the registration is still there.
        SERVICE_STATUS status{};
        if (QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_STOPPED) {
            if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
                // The worker's own stop path tells the master what it had running and stops those
                // instances first, so this is not instant. Bounded rather than open-ended: a worker
                // that will not stop should not leave an operator's command hanging, and the
                // deletion below is still the right thing to do.
                for (int waited = 0; waited < 30 && status.dwCurrentState != SERVICE_STOPPED; ++waited) {
                    std::this_thread::sleep_for(std::chrono::seconds{1});
                    if (!QueryServiceStatus(svc, &status)) break;
                }
            }
        }

        const bool deleted = DeleteService(svc);
        const DWORD error = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);

        if (!deleted) return LastErrorText("DeleteService", error);
        return "";
    }

}// namespace Euclid::Worker::Service

#endif
