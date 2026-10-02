// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#if defined(_WIN32)

// C++ includes
#include <string>

namespace Euclid::Worker::Service {

    /**
     * @brief The name the Service Control Manager knows this service by.
     *
     * One constant rather than a literal in four places: it is the name passed to CreateService,
     * the name RegisterServiceCtrlHandlerEx must be given to find the status handle the SCM already
     * created, the name the dispatch table carries, and the name an operator types after "sc". A
     * mismatch between any two of them is a service that installs and then fails to start with
     * error 1053, which says only that it did not respond in time.
     *
     * Matches the service the MSI declares (dist/win32/msi/euclid-wrk.wxs), so "--install" and the
     * package produce the same service rather than two that fight over the same binary.
     */
    inline constexpr auto Name = "euclid-wrk";

    /**
     * @brief Registers the running executable as the Windows service "euclid-wrk"
     *        (SERVICE_AUTO_START, running as Local System), with the configuration and credentials
     *        paths baked into its start command line.
     *
     * For anyone deploying the binaries without the MSI. The package installs the same service
     * declaratively (ServiceInstall), which is the better route when there is a package: Windows
     * then owns installing, starting, stopping and removing it as steps of a transaction that rolls
     * back. This is the equivalent for a hand-built tree, and the manager has had one for the same
     * reason - see Platform::InstallService.
     *
     * Deliberately not started. A worker refuses to start without credentials, and a fresh install
     * has none - see the --credentials comment in main.cpp. Start it once the file is in place.
     *
     * @param configFilePath  passed to the service as --config; may be empty, in which case no
     *                        --config is baked in and the worker runs on its defaults
     * @param credentialsFilePath passed as --credentials; may be empty, in which case the worker
     *                        falls back to the invoking user's own credentials file, which for a
     *                        service means Local System's
     * @return empty string on success; a human-readable error message otherwise (e.g.
     *         "already exists", "access denied - run as Administrator").
     */
    std::string Install(const std::string &configFilePath, const std::string &credentialsFilePath);

    /**
     * @brief Stops the service if it is running and removes it from the Service Control Manager.
     *
     * Stopped first, because DeleteService on a running service only marks it for deletion and the
     * registration survives until the process exits - which, for a service nobody stops, is until
     * the host reboots. An operator who then runs --install gets "already installed" and no
     * explanation.
     *
     * @return empty string on success; a human-readable error message otherwise.
     */
    std::string Uninstall();

}// namespace Euclid::Worker::Service

#endif
