#pragma once

// C++ includes
#include <fstream>
#include <iostream>
#include <sstream>
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
#include <euclid/dto/ess/CreateSecretRequest.h>
#include <euclid/dto/ess/ListSecretsRequest.h>
#include <euclid/dto/ess/SecretNameRequest.h>
#include <euclid/dto/ess/UpdateSecretRequest.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the ess (Euclid Secrets Store) module (e.g. "ess get-secret --name db-password").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EssCli final : BaseCli {

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
        explicit EssCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Stores a new secret.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int createSecret(const std::vector<std::string> &args) const;

        /**
         * @brief Reads a secret's value.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getSecret(const std::vector<std::string> &args) const;

        /**
         * @brief Lists the stored secrets, without their values.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listSecrets(const std::vector<std::string> &args) const;

        /**
         * @brief Rotates a secret's value, changes its description, or moves it to another key.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int updateSecret(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes a secret.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteSecret(const std::vector<std::string> &args) const;

        /**
         * @brief Reads the value a command was given: --value, or --value-file, or standard input.
         *
         * @par
         * Three ways because the obvious one is the worst: a value on the command line is in the
         * process list while it runs and in the shell history afterwards. The other two exist so
         * that a real secret never has to be typed there.
         *
         * @param vm parsed command line
         * @param value receives the value
         * @return true if a value was obtained; false after printing why it was not
         */
        [[nodiscard]]
        static bool readValue(const boost::program_options::variables_map &vm, std::string &value);

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

}// namespace Euclid::CLI
