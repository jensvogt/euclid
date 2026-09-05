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
     * @brief Processes commands for the "eag" (Euclid API gateway) module (e.g. "eag list-routes").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EagCli final : BaseCli {

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
        explicit EagCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Publishes a path prefix through the gateway, pointing it at an application.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int createRoute(const std::vector<std::string> &args) const;

        /**
         * @brief Changes an existing route, leaving anything not named alone.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int updateRoute(const std::vector<std::string> &args) const;

        /**
         * @brief Lists the configured routes.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listRoutes(const std::vector<std::string> &args) const;

        /**
         * @brief Shows one route's definition.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getRoute(const std::vector<std::string> &args) const;

        /**
         * @brief Removes a route, taking its path out of service.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteRoute(const std::vector<std::string> &args) const;

        /**
         * @brief Euclid endpoint
         */
        std::string _endpoint;

        /**
         * @brief Bearer token used to authenticate requests, or empty if not logged in.
         */
        Credentials::Entry _authentication;

        /**
         * @brief Pretty print flag
         */
        bool _pretty;

        /**
         * @brief Path to an additional PEM CA certificate to trust, or empty to use only the
         * system trust store.
         */
        std::string _caCertPath;
    };

}
