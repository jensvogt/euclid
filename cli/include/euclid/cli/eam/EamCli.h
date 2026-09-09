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
         * @brief Login through the configured OIDC provider, e.g. OneLogin.
         *
         * @par
         * Opens a browser at the provider, catches the redirect that comes back on a loopback
         * listener this process owns for the duration, and hands the code to EAM - which is what
         * makes the CLI a public client that never sees a password, and why the listener is on
         * 127.0.0.1 and lives for one login.
         *
         * @param nameSpace namespace to make active for the session, or empty for none.
         * @return the process exit code.
         */
        [[nodiscard]]
        int loginWithOidc(const std::string &nameSpace) const;

        /**
         * @brief Login through the configured SAML identity provider.
         *
         * @par
         * The same shape as loginWithOidc(): a browser does the talking and a loopback listener
         * catches what comes back. What comes back differs - a SAML assertion is posted to the
         * gateway, not to this process, so euclid answers the browser with a page that posts the
         * finished session here instead.
         *
         * @param nameSpace namespace to make active for the session, or empty for none.
         * @return the process exit code.
         */
        [[nodiscard]]
        int loginWithSaml(const std::string &nameSpace) const;

        /**
         * @brief Login through OneLogin's API, with no browser.
         *
         * @par
         * OneLogin can mint a SAML assertion over its API for a person who presents a password and
         * a second factor - which is what makes an unattended login possible. The password and the
         * one-time code go to OneLogin and nowhere else; euclid is handed the finished assertion
         * and verifies it exactly as it verifies one that came through a browser.
         *
         * @param nameSpace namespace to make active for the session, or empty for none.
         * @param application which configured OneLogin application to ask for, or empty for the
         * only one.
         * @param user OneLogin user to sign in as, or empty for the configured one.
         * @param oneTimeCode second factor, or empty to compute it from the configured TOTP
         * secret, or to be asked for it.
         * @param device which enrolled second factor to use, by id or type, or empty to be asked
         * when there is more than one.
         * @param givenPassword the OneLogin password as typed on the command line, or empty to
         * take it from the environment, the configuration or the terminal.
         * @param showAssertion print what the assertion says and stop, rather than logging in -
         * for setting the server's SAML block up from a real assertion.
         * @return the process exit code.
         */
        [[nodiscard]]
        int loginWithOneLogin(const std::string &nameSpace, const std::string &application,
                              const std::string &user, const std::string &oneTimeCode,
                              const std::string &device, const std::string &givenPassword,
                              bool showAssertion) const;

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