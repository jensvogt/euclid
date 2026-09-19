// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/json.hpp>
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/cli/BaseCli.h>
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/help/CliHelp.h>
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/core/LogStream.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "eap" (Euclid application) module (e.g. "eap list-applications").
     *
     * @author jensvogt47\@gmail.com
     */
    class EapCli final : BaseCli {

    public:

        /**
         * @brief Constructs the handler.
         *
         * @param endpoint  Euclid server endpoint
         * @param authentication authentication including the bearer token used to authenticate requests
         * @param pretty pretty print output
         * @param caCertPath if non-empty, path to a PEM CA certificate trusted in addition to the
         * system trust store, e.g. for self-signed development certificates
         */
        explicit EapCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

        /**
         * @brief Every action this module takes, with the one-line summary each is listed by.
         *
         * @return the actions, in the order "help" lists them
         */
        static const std::vector<std::pair<std::string, std::string> > &Actions();

        /**
         * @brief Dispatches to the handler for the given action. Returns the process exit code.
         *
         * @param action action string
         * @param args list of action arguments
         */
        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

    private:

        /**
         * @brief Defines a new application from an artifact stored in an ESM bucket.
         *
         * The application is created stopped; "start-application" is what asks the manager to run it.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int createApplication(const std::vector<std::string> &args) const;

        /**
         * @brief Changes an existing application's definition.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int updateApplication(const std::vector<std::string> &args) const;

        /**
         * @brief Deploys a new build of an application from a local file.
         *
         * The two commands this replaces - uploading the artifact, then updating the definition so
         * the manager restarts onto it - are always run together and always in that order, and
         * doing only the first is a deployment that silently does not happen.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int redeployApplication(const std::vector<std::string> &args) const;

        /**
         * @brief Lists applications, with how many instances of each are actually running.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listApplications(const std::vector<std::string> &args) const;

        /**
         * @brief Shows one application's definition and observed state.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getApplication(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes an application, stopping it if it is running.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteApplication(const std::vector<std::string> &args) const;

        /**
         * @brief Asks the manager to run an application.
         *
         * @param args command line arguments
         * @param start true to start, false to stop
         * @return ok
         */
        [[nodiscard]]
        int setState(const std::vector<std::string> &args, bool start) const;

        /**
         * @brief Asks the manager to start an application's instances again.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int restartApplication(const std::vector<std::string> &args) const;

        /**
         * @brief Sets the level an application's own output is logged at.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int setLogLevel(const std::vector<std::string> &args) const;

        std::string _endpoint;
        Credentials::Entry _authentication;
        bool _pretty;
        std::string _caCertPath;
    };

}// namespace Euclid::CLI
