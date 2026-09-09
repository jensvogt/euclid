//
// Created by vogje01 on 9/9/26.
//

#pragma once

// C++ includes
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::CLI {

    /**
     * @brief What is needed to have OneLogin mint a SAML assertion over its API.
     *
     * @par
     * Read from euclid.cli.onelogin, overridden by environment variables, overridden by what is
     * typed at the terminal - so an installation can put the parts that are not personal in a file,
     * a person can keep their password out of it, and a scheduled job can supply everything from
     * its environment.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct OneLoginConfiguration {

        /**
         * @brief OneLogin subdomain, e.g. "libri-gmbh" for libri-gmbh.onelogin.com.
         */
        std::string subDomain;

        /**
         * @brief The API's base URL, when it is not "https://\<sub-domain\>.onelogin.com".
         *
         * @par
         * For a tenant on one of OneLogin's regional API hosts, and for pointing this at something
         * other than OneLogin in a test.
         */
        std::string baseUrl;

        /**
         * @brief The API credential pair from Administration → Developers → API Credentials.
         *
         * @par
         * Not an OIDC client and not a SAML entity: these authenticate the *program* to OneLogin's
         * API, which then authenticates the person with the password and second factor below.
         */
        std::string clientId;

        /**
         * @brief Secret half of the API credential pair.
         */
        std::string clientSecret;

        /**
         * @brief The person signing in, as OneLogin knows them.
         */
        std::string user;

        /**
         * @brief Their password. Left empty in a configuration file, and asked for instead.
         */
        std::string password;

        /**
         * @brief The base32 TOTP secret, when the login is to run unattended. Empty means the code
         * is asked for at the terminal.
         */
        std::string otpKey;

        /**
         * @brief The OneLogin application to get an assertion for, by name - "int", "prod",
         * whatever the environments are called. Empty uses the only one configured, or the one
         * named "default".
         */
        std::string application;

        /**
         * @brief Which enrolled second factor to use, by device ID or by the name OneLogin gives
         * its type ("Google Authenticator", "OneLogin Protect").
         *
         * @par
         * Only matters for somebody with more than one enrolled, and then it matters a lot: a code
         * read from one authenticator will not verify against another, and the failure OneLogin
         * reports for that says nothing about which device it was expecting. Empty asks, where
         * there is a terminal to ask at.
         */
        std::string device;

        /**
         * @brief Application IDs by name, from euclid.cli.onelogin.app-ids.
         */
        std::map<std::string, std::string> applications;

        /**
         * @brief Reads what the configuration file and the environment say. Nothing is prompted
         * for here; see OneLoginClient::Authenticate().
         *
         * @return the configuration, with whatever was found.
         */
        static OneLoginConfiguration Read();

        /**
         * @brief The application ID to ask for, resolved from application/applications.
         *
         * @return the ID, or empty if the configuration names none.
         */
        [[nodiscard]]
        std::string ApplicationId() const;

        /**
         * @brief What is missing before a login can be attempted, one message per problem. The
         * password and the one-time code are not checked here - those are asked for.
         */
        [[nodiscard]]
        std::vector<std::string> Validate() const;
    };

    /**
     * @brief Raised when OneLogin cannot be reached, or refuses.
     */
    struct OneLoginError final : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Gets a SAML assertion out of OneLogin's API, with no browser involved.
     *
     * @par
     * The flow OneLogin publishes for exactly this: authenticate the program with its API
     * credentials, ask for an assertion on behalf of a person with their password, satisfy the
     * second factor if one is required, and receive the same signed assertion the browser flow
     * would have posted. Euclid then verifies it the way it verifies any other - the assertion is
     * what is trusted, not the path it arrived by.
     *
     * @par
     * One consequence is worth knowing: an assertion minted this way answers no authentication
     * request of euclid's, so it is unsolicited as far as the service provider is concerned, and
     * the installation has to allow those (saml.allow-idp-initiated).
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class OneLoginClient {

    public:

        /**
         * @brief One enrolled second factor.
         */
        struct Device {

            /**
             * @brief OneLogin's identifier for it, which is what verify_factor is given.
             */
            std::string id;

            /**
             * @brief What kind it is - "OneLogin Protect", "Google Authenticator" - which is the
             * only part a person recognises.
             */
            std::string type;
        };

        /**
         * @brief Constructs the client.
         *
         * @param config where to go and who to say we are.
         * @param caCertPath extra CA certificate to trust, or empty for the system store.
         */
        explicit OneLoginClient(OneLoginConfiguration config, std::string caCertPath = {});

        /**
         * @brief Asked for a one-time code, when there is neither one to hand nor a secret to
         * compute it from. What it is given is what the person typed, or empty if nobody could be
         * asked.
         *
         * @par
         * A callback rather than a value because the code is short-lived: asking for one up front
         * would mean the person types a code, waits for a password round trip, and finds it has
         * expired. This is called at the moment OneLogin says it wants one, and only then - an
         * account without a second factor is never asked.
         */
        using OneTimeCodeProvider = std::function<std::string(const std::string &deviceType)>;

        /**
         * @brief Asked which device to use, when OneLogin offers several and the configuration
         * does not say. Returns an index into @p devices, or anything out of range to take the
         * first.
         */
        using DeviceChooser = std::function<std::size_t(const std::vector<Device> &devices)>;

        /**
         * @brief Runs the whole exchange and returns the assertion.
         *
         * @param password the person's password.
         * @param oneTimeCode the second factor, or empty to compute it from the configured TOTP
         * secret, or to ask for it through @p askForCode.
         * @param askForCode consulted when a code is wanted and neither of the other two is there;
         * may be empty, in which case the login fails with an explanation.
         * @return the base64 SAML assertion, ready to post to euclid's assertion consumer service.
         * @throws OneLoginError if OneLogin refuses or cannot be reached.
         */
        [[nodiscard]]
        std::string SamlAssertion(const std::string &password, const std::string &oneTimeCode,
                                  const OneTimeCodeProvider &askForCode = {},
                                  const DeviceChooser &chooseDevice = {}) const;

        /**
         * @brief What OneLogin answered when asked for an assertion: either the assertion, or a
         * demand for a second factor.
         *
         * Split out from the request so that the shapes OneLogin's API answers in - which differ
         * between its versions - can be tested without an account.
         */
        struct AssertionAnswer {

            /**
             * @brief The base64 assertion, when there was nothing more to satisfy.
             */
            std::string assertion;

            /**
             * @brief The token identifying this half-finished login, when there was.
             */
            std::string stateToken;

            /**
             * @brief Every device OneLogin will accept a factor from, in the order it offered
             * them.
             *
             * @par
             * All of them, not just the first: an account with an authenticator app and a hardware
             * token has two, and picking one of them silently is how a login fails with "Failed
             * authentication with this factor" for a code that was perfectly correct - for the
             * other device.
             */
            std::vector<Device> devices;

            /**
             * @brief What OneLogin said, for the log or the error message.
             */
            std::string message;
        };

        /**
         * @brief Reads OneLogin's answer to a saml_assertion request.
         *
         * @param answer the parsed JSON body.
         * @return what it says.
         * @throws OneLoginError if it says neither of the two things it should.
         */
        [[nodiscard]]
        static AssertionAnswer ParseAssertionAnswer(const boost::json::value &answer);

    private:

        OneLoginConfiguration _config;
        std::string _caCertPath;
    };

}// namespace Euclid::CLI
