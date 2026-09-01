#pragma once

// C++ includes
#include <iostream>
#include <string>
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

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "eap" (Euclid application) module (e.g. "eap list-applications").
     *
     * @author jens.vogt\@opitz-consulting.com
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

        std::string _endpoint;
        Credentials::Entry _authentication;
        bool _pretty;
        std::string _caCertPath;
    };

}// namespace Euclid::CLI
