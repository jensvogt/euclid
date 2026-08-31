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
     * @brief Processes commands for the "ets" (Euclid transfer server) module (e.g. "ets list-servers").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EtsCli final : BaseCli {

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
        explicit EtsCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Defines a new FTP or SFTP transfer server fronting an ESM bucket.
         *
         * The server is created stopped; "start-server" is what asks the manager to run it.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int createServer(const std::vector<std::string> &args) const;

        /**
         * @brief Changes an existing server's settings. Only the options actually given are
         * altered, so a single setting can be changed without resending the whole definition.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int updateServer(const std::vector<std::string> &args) const;

        /**
         * @brief Lists the defined transfer servers, with both their desired and observed state.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listServers(const std::vector<std::string> &args) const;

        /**
         * @brief Shows one transfer server's full definition.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getServer(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes a transfer server definition, stopping it if it is running.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteServer(const std::vector<std::string> &args) const;

        /**
         * @brief Sets a server's desired state, which the manager then reconciles.
         *
         * Backs both "start-server" and "stop-server", since the two differ only in the action
         * they send and in what they print.
         *
         * @param action either "start-server" or "stop-server".
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int setServerState(const std::string &action, const std::vector<std::string> &args) const;

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
