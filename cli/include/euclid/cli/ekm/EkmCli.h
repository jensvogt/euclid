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
#include <euclid/dto/ekm/CreateKeyRequest.h>
#include <euclid/dto/ekm/DeleteKeyRequest.h>
#include <euclid/dto/ekm/RevokeKeyRequest.h>
#include <euclid/dto/ekm/SetKeyDescriptionRequest.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the ekm (Euclid Key Management) module (e.g. "ekm create-key --type aes256").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EkmCli final : BaseCli {

    public:

        /**
         * @brief Constructs the handler.
         *
         * @param endpoint  Euclid server endpoint
         * @param authentication authentication including the bearer token used to authenticate requests, e.g. for register/list-users;
         * @param pretty pretty print output;
         * empty if the caller isn't logged in yet
         * @param caCertPath if non-empty, path to a PEM CA certificate trusted in addition to the
         * system trust store, e.g. for self-signed development certificates
         */
        explicit EkmCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Create a new key
         *
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int createKey(const std::vector<std::string> &args) const;

        /**
         * @brief List all available keys
         *
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int listKeys(const std::vector<std::string> &args) const;

        /**
         * @brief Schedules a key for permanent deletion after a grace period (default 7 days)
         *
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteKey(const std::vector<std::string> &args) const;

        /**
         * @brief Revokes a key: blocks it from further encryption while leaving decryption of
         * previously-encrypted data unaffected
         *
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int revokeKey(const std::vector<std::string> &args) const;

        /**
         * @brief Replaces what a key says it is for.
         *
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int setKeyDescription(const std::vector<std::string> &args) const;

        /**
         * @brief Encrypts a file or stdin with a key, writing the ciphertext to a file or stdout
         *
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int encrypt(const std::vector<std::string> &args) const;

        /**
         * @brief Decrypts a file or stdin with a key, writing the plaintext to a file or stdout
         *
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int decrypt(const std::vector<std::string> &args) const;

        /**
         * @brief Shared implementation of encrypt()/decrypt(): both read a key ID plus a file or
         * stdin, POST the raw bytes to the given action, and write the raw response to a file or
         * stdout. Only the action name and its help text differ between the two.
         *
         * @param action "encrypt" or "decrypt"
         * @param caption short caption for the options group, e.g. "encrypt a file or stdin with a key"
         * @param description longer sentence shown in the action's DESCRIPTION section
         * @param args command arguments
         * @return ok
         */
        [[nodiscard]]
        int runTransform(const std::string &action, const std::string &caption, const std::string &description, const std::vector<std::string> &args) const;

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