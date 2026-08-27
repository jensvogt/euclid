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
#include <euclid/dto/eam/CreateAccessKeyResponse.h>
#include <euclid/dto/eam/ChangeNamespaceRequest.h>
#include <euclid/dto/eam/CreateAccountRequest.h>
#include <euclid/dto/eam/CreateNamespaceRequest.h>
#include <euclid/dto/eam/CreateUserGroupRequest.h>
#include <euclid/dto/eam/DeleteAccessKeyRequest.h>
#include <euclid/dto/eam/DeleteAccountRequest.h>
#include <euclid/dto/eam/DeleteNamespaceRequest.h>
#include <euclid/dto/eam/DeleteUserGroupRequest.h>
#include <euclid/dto/eam/DeleteUserRequest.h>
#include <euclid/dto/eam/GrantNamespaceAccessRequest.h>
#include <euclid/dto/eam/ListAccessKeysResponse.h>
#include <euclid/dto/eam/ListAccountsRequest.h>
#include <euclid/dto/eam/ListNamespacesRequest.h>
#include <euclid/dto/eam/ListUserGroupsRequest.h>
#include <euclid/dto/eam/ListUserRequest.h>
#include <euclid/dto/eam/ListUserResponse.h>
#include <euclid/dto/eam/LoginRequest.h>
#include <euclid/dto/eam/LoginResponse.h>
#include <euclid/dto/eam/RegisterRequest.h>
#include <euclid/dto/eam/RevokeNamespaceAccessRequest.h>
#include <euclid/dto/eam/UserGroupAddUserRequest.h>
#include <euclid/dto/eam/UserGroupRemoveUserRequest.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "access" module (e.g. "access login --user <u> --password <p>").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EamCli final : BaseCli {

    public:

        /**
         * @brief Constructs the handler.
         *
         * @param endpoint  Euclid server endpoint
         * @param authentication bearer token used to authenticate requests, e.g. for register/list-users;
         * @param pretty pretty print output;
         * empty if the caller isn't logged in yet
         * @param caCertPath if non-empty, path to a PEM CA certificate trusted in addition to the
         * system trust store, e.g. for self-signed development certificates
         */
        explicit EamCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

        /**
         * @brief Dispatches to the handler for the given action. Returns the process exit code.
         */
        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

    private:

        /**
         * @brief Login of a user
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int login(const std::vector<std::string> &args) const;

        /**
         * @brief Register a new user
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int registerUser(const std::vector<std::string> &args) const;

        /**
         * @brief List all available users
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int listUsers(const std::vector<std::string> &args) const;

        /**
         * @brief Delete an existing user
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int deleteUser(const std::vector<std::string> &args) const;

        /**
         * @brief Creates a new SigV4 access key for the caller and stores it locally
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int createAccessKey(const std::vector<std::string> &args) const;

        /**
         * @brief Lists the caller's own access keys
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int listAccessKeys(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes one of the caller's own access keys
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int deleteAccessKey(const std::vector<std::string> &args) const;

        /**
         * @brief Creates a new user group
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int createUserGroup(const std::vector<std::string> &args) const;

        int listUserGroups(const std::vector<std::string> &args) const;

        int addUserToUserGroup(const std::vector<std::string> &args) const;

        int removeUserFromUserGroup(const std::vector<std::string> &args) const;

        int deleteUserGroup(const std::vector<std::string> &args) const;

        /**
         * @brief Creates a new account
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int createAccount(const std::vector<std::string> &args) const;

        /**
         * @brief Lists accounts
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int listAccounts(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes an existing account
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int deleteAccount(const std::vector<std::string> &args) const;

        /**
         * @brief Creates a new namespace under an account
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int createNamespace(const std::vector<std::string> &args) const;

        /**
         * @brief Lists namespaces under an account
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int listNamespaces(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes an existing namespace
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int deleteNamespace(const std::vector<std::string> &args) const;

        /**
         * @brief Grants a user access to a namespace within an account
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int grantNamespaceAccess(const std::vector<std::string> &args) const;

        /**
         * @brief Revokes a user's access to a namespace within an account
         *
         * @param args action arguments
         * @return
         */
        [[nodiscard]]
        int revokeNamespaceAccess(const std::vector<std::string> &args) const;

        /**
         * @brief Switches the caller's active namespace for this session (persisted in
         * ~/.euclid/credentials, sent as x-euclid-namespace on every subsequent command).
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int changeNamespace(const std::vector<std::string> &args) const;

        /**
         * @brief Euclid endpoint
         */
        std::string _endpoint;

        /**
         * @brief Authentication including the bearer token
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