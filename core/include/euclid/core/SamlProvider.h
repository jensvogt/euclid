//
// Created by vogje01 on 9/9/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/Federation.h>

namespace Euclid::Core {

    /**
     * @brief Everything needed to be a SAML 2.0 service provider to one identity provider, read
     * from euclid.modules.\<module\>.saml.
     *
     * @par
     * SAML has no discovery worth the name - an installation is configured from the metadata its
     * identity provider publishes, by hand, once. What is unavoidable is the certificate: an
     * assertion is trusted because it is signed by a key euclid was told to expect, and there is
     * no other basis for trusting it.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct SamlConfiguration {

        /**
         * @brief Whether SAML login is offered at all.
         */
        bool enabled{false};

        /**
         * @brief This installation's entity ID, as registered with the identity provider. Also the
         * audience an assertion must name, which is what stops one minted for another service
         * being replayed here.
         */
        std::string entityId;

        /**
         * @brief Where the identity provider posts the assertion, e.g.
         * "https://euclid.example.com:5566/eam/saml/acs". Checked against the assertion's
         * Recipient, so it has to be the URL the provider actually posts to.
         */
        std::string acsUrl;

        /**
         * @brief The identity provider's entity ID, as an assertion's Issuer must state it.
         */
        std::string idpEntityId;

        /**
         * @brief The identity provider's single sign-on URL, HTTP-Redirect binding.
         */
        std::string idpSsoUrl;

        /**
         * @brief The identity provider's signing certificate, PEM, inline in the configuration.
         */
        std::string idpCertificate;

        /**
         * @brief Path to the identity provider's signing certificate, when it is a file rather
         * than inline. One of this and idpCertificate has to be set.
         */
        std::string idpCertificateFile;

        /**
         * @brief Assertion attribute naming the euclid user ID, by Name or FriendlyName. Empty
         * uses the NameID, which for most providers is the email address or the login name.
         */
        std::string usernameAttribute;

        /**
         * @brief Assertion attribute carrying the email address.
         */
        std::string emailAttribute{"email"};

        /**
         * @brief Account new users are provisioned into. Empty falls back to the first entry of
         * euclid.account-ids.
         */
        std::string accountId;

        /**
         * @brief Whether an identity with no matching euclid user gets one created on the spot.
         */
        bool jitProvisioning{true};

        /**
         * @brief Whether a federated login may adopt an existing euclid user of the same name that
         * is not yet tied to any provider. Off, for the same reason as its OIDC counterpart: with
         * it on, whoever the provider calls "admin" becomes euclid's admin.
         */
        bool linkExistingUsers{false};

        /**
         * @brief Whether an assertion that answers no request of ours is accepted - somebody
         * clicking the euclid tile in their provider's portal.
         *
         * @par
         * Off by default. An unsolicited assertion cannot be tied to a login this installation
         * started, so the only things standing between it and a session are its signature, its
         * validity window and the replay check. That is how the profile is meant to work and
         * plenty of installations rely on it, but it is strictly weaker than the SP-initiated
         * flow, so it is asked for rather than assumed.
         */
        bool allowIdpInitiated{false};

        /**
         * @brief How much clock difference between here and the provider to tolerate when checking
         * an assertion's validity window.
         */
        std::chrono::seconds clockSkew{60};

        /**
         * @brief How long an authentication may stay in flight - the lifetime sealed into the
         * RelayState.
         */
        std::chrono::seconds stateTtl{std::chrono::minutes(10)};

        /**
         * @brief URL prefixes a browser may be sent on to once signed in; see
         * Core::IsReturnToAllowed.
         */
        std::vector<std::string> returnToPrefixes;

        /**
         * @brief Reads the configuration of one module's SAML block.
         *
         * @param module module whose block to read, e.g. "eam".
         * @return the configuration, with enabled=false if the block is absent.
         */
        static SamlConfiguration FromConfiguration(const std::string &module);

        /**
         * @brief What is missing or wrong before an assertion can be *verified*, one message per
         * problem.
         *
         * @par
         * Deliberately not everything: an installation that only ever consumes assertions - one
         * whose people get them from their provider's API rather than through a browser - needs no
         * SSO URL, and refusing its logins for the want of one would be refusing them over a
         * setting nothing was going to read.
         *
         * @return an empty vector when assertions can be verified.
         */
        [[nodiscard]]
        std::vector<std::string> Validate() const;

        /**
         * @brief What is additionally missing before a login can be *started* from here - the
         * browser flow, which has to know where to send somebody.
         *
         * @return Validate()'s problems, plus any that only matter for starting a login.
         */
        [[nodiscard]]
        std::vector<std::string> ValidateForAuthentication() const;

        /**
         * @brief The identity provider's certificate, from wherever it is configured.
         *
         * @return the PEM text.
         * @throws SamlError if it is configured as a file that cannot be read.
         */
        [[nodiscard]]
        std::string IdpCertificatePem() const;

        /**
         * @brief Whether a browser may be sent on to @p returnTo after signing in.
         */
        [[nodiscard]]
        bool IsReturnToAllowed(const std::string &returnTo) const;
    };

    /**
     * @brief What an assertion says about itself, read without checking any of it.
     *
     * @par
     * For setting an installation up. The three values euclid has to be configured with - the
     * issuer to expect, the audience it answers to, the endpoint it is addressed as - are all
     * stated in any assertion the provider will send, and reading them off one is a great deal
     * easier than finding them in an administration console. Nothing here is verified, and it must
     * not be treated as if it were: it is what an unauthenticated document claims.
     */
    struct SamlDescription {

        /**
         * @brief The Issuer, which is what saml.idp-entity-id has to be.
         */
        std::string issuer;

        /**
         * @brief The Audience, which is what saml.entity-id has to be.
         */
        std::string audience;

        /**
         * @brief The subject confirmation's Recipient, which is what saml.acs-url has to be.
         */
        std::string recipient;

        /**
         * @brief The Destination the response names, if any.
         */
        std::string destination;

        /**
         * @brief The NameID - who this says the person is.
         */
        std::string nameId;

        /**
         * @brief When the assertion stops being valid.
         */
        std::string notOnOrAfter;

        /**
         * @brief Whether it carries a signature at all. Whether that signature is any good is
         * SamlResponseVerifier::Verify()'s question, not this one's.
         */
        bool hasSignature{false};

        /**
         * @brief The attribute statement, as "name=value" pairs - where a username or an email
         * would be found, for saml.username-attribute.
         */
        std::vector<std::string> attributes;
    };

    /**
     * @brief Raised for every way a SAML exchange can fail.
     */
    struct SamlError final : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Verifies a SAML response, with no I/O of its own.
     *
     * @par
     * Separated from everything else because this is where a service provider is broken or not.
     * The signature is checked by xmlsec rather than by hand, and around it sit the rules that
     * signature checking alone does not give you - the ones behind every XML signature wrapping
     * advisory of the last fifteen years:
     *
     * @par
     * - the document must contain exactly one assertion, so there is no second one to consume;
     * - every ID must be unique, so a reference cannot be made to resolve to a different element
     *   than the one it appears to name;
     * - the signature must be a child of the element it covers, and its single reference must name
     *   that element by ID, so a signature cannot be lifted from something else;
     * - the key is the one euclid was configured with, never one the document offers;
     * - only exclusive canonicalisation, SHA-256 and RSA-SHA256 are enabled;
     * - and the claims are read from the element that was verified, not from the document again.
     */
    class SamlResponseVerifier {

    public:

        /**
         * @brief Checks a response's signature and contents, and extracts the identity.
         *
         * @param responseXml the decoded SAML response document.
         * @param config the issuer, audience and certificate to require.
         * @param expectedInResponseTo the ID of the AuthnRequest this answers, or empty for an
         * unsolicited (IdP-initiated) assertion, which is only accepted when the configuration
         * allows it.
         * @param error set to why verification failed, untouched on success.
         * @return the identity, or std::nullopt if the response is not acceptable.
         */
        [[nodiscard]]
        static std::optional<FederatedIdentity> Verify(const std::string &responseXml, const SamlConfiguration &config,
                                                       const std::string &expectedInResponseTo, std::string &error);

        /**
         * @brief Reads what a response says about itself, checking none of it.
         *
         * @param responseXml the decoded SAML response document.
         * @return what it claims, or std::nullopt if it is not a SAML response at all.
         */
        [[nodiscard]]
        static std::optional<SamlDescription> Describe(const std::string &responseXml);
    };

    /**
     * @brief Drives SAML 2.0 web browser SSO against an identity provider.
     *
     * @par
     * Euclid is the service provider: the provider authenticates the person and posts a signed
     * assertion back, which is turned into an ordinary euclid session - the same one a password
     * login produces. Requests go out over the HTTP-Redirect binding and assertions come back over
     * HTTP-POST, which is what every provider offers and what OneLogin's defaults use.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class SamlProvider {

    public:

        /**
         * @brief Where to send the browser, and what has to come back with it.
         */
        struct Authentication {

            /**
             * @brief The provider's SSO URL with the deflated, encoded AuthnRequest on it.
             */
            std::string url;

            /**
             * @brief The sealed RelayState, also present in url.
             */
            std::string relayState;

            /**
             * @brief The AuthnRequest's ID, which the assertion has to answer.
             */
            std::string requestId;
        };

        /**
         * @brief Constructs the provider.
         *
         * @param config the provider configuration.
         * @param stateSecret secret the RelayState is sealed with, i.e. the installation's JWT
         * secret.
         */
        SamlProvider(SamlConfiguration config, std::string stateSecret);

        /**
         * @brief Starts a login: builds an AuthnRequest and the redirect that carries it.
         *
         * @param returnTo where the caller wants the browser to end up afterwards, or empty.
         * @return the redirect URL and its RelayState.
         * @throws SamlError if the request cannot be built.
         */
        [[nodiscard]]
        Authentication BeginAuthentication(const std::string &returnTo) const;

        /**
         * @brief Finishes a login: verifies what the provider posted back.
         *
         * @param samlResponse the base64 "SAMLResponse" form field.
         * @param relayState the "RelayState" form field, as sealed by BeginAuthentication(); empty
         * for an unsolicited assertion.
         * @param returnTo set to where the browser should be sent on to, if the RelayState says so.
         * @return the verified identity.
         * @throws SamlError if the response is missing, unreadable or unacceptable.
         */
        [[nodiscard]]
        FederatedIdentity Consume(const std::string &samlResponse, const std::string &relayState, std::string &returnTo) const;

        /**
         * @brief This installation's SP metadata, for pasting into the provider.
         *
         * @return the EntityDescriptor XML.
         */
        [[nodiscard]]
        std::string Metadata() const;

        /**
         * @brief The configuration this provider was built with.
         */
        [[nodiscard]]
        const SamlConfiguration &Config() const { return _config; }

    private:

        SamlConfiguration _config;
        std::string _stateSecret;
    };

}// namespace Euclid::Core
