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
     * @brief Everything needed to talk to one OpenID Connect provider, read from
     * euclid.modules.\<module\>.oidc.
     *
     * @par
     * Only issuer, client-id and client-secret have to be configured. The three endpoints below
     * them are discovered from the issuer's /.well-known/openid-configuration, which is what a
     * provider publishes them for; they exist here as overrides for a deployment whose provider
     * cannot be reached for discovery, or which pins them deliberately.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct OidcConfiguration {

        /**
         * @brief Whether OIDC login is offered at all. False leaves euclid exactly as it was:
         * password login only, and the OIDC actions refused with "not enabled".
         */
        bool enabled{false};

        /**
         * @brief Issuer URL, e.g. "https://example.onelogin.com/oidc/2". Also the value the "iss"
         * claim of an ID token must carry, which is why it is not merely a base address.
         */
        std::string issuer;

        /**
         * @brief Client ID registered with the provider, and the audience an ID token must name.
         */
        std::string clientId;

        /**
         * @brief Client secret registered with the provider. Sent to the token endpoint only.
         */
        std::string clientSecret;

        /**
         * @brief Where the provider sends the browser back to. Used when a caller does not supply
         * one of its own - the CLI does (its loopback listener), a browser-driven flow does not.
         */
        std::string redirectUri;

        /**
         * @brief Space-separated scopes to request; "openid" is added if absent, since without it
         * the provider has no reason to return an ID token at all.
         */
        std::string scopes{"openid profile email"};

        /**
         * @brief Authorization endpoint, discovered from the issuer when empty.
         */
        std::string authorizationEndpoint;

        /**
         * @brief Token endpoint, discovered from the issuer when empty.
         */
        std::string tokenEndpoint;

        /**
         * @brief JWKS URI the ID token's signing key is fetched from, discovered when empty.
         */
        std::string jwksUri;

        /**
         * @brief ID token claim naming the euclid user ID, e.g. "preferred_username".
         *
         * @par
         * Deliberately not "sub": a provider's subject is an opaque identifier, and a euclid user
         * ID appears in ERNs, log lines and access-key ownership, where an operator has to
         * recognise it. The subject is still what the account is keyed on internally (see
         * FederatedIdentity::subject), so renaming a person in the provider does not orphan their user.
         */
        std::string usernameClaim{"preferred_username"};

        /**
         * @brief ID token claim carrying the user's email address.
         */
        std::string emailClaim{"email"};

        /**
         * @brief Account new users are provisioned into. Empty falls back to the first entry of
         * euclid.account-ids, the same account the bootstrap admin lands in.
         */
        std::string accountId;

        /**
         * @brief Whether an identity with no matching euclid user gets one created on the spot.
         *
         * @par
         * With this off, a person the provider authenticated is still refused unless an
         * administrator has already created their user - which is the right answer for an
         * installation that wants its user list to be an explicit thing.
         */
        bool jitProvisioning{true};

        /**
         * @brief Whether a federated login may adopt an existing euclid user whose user ID it
         * matches but which is not yet tied to any provider subject.
         *
         * @par
         * Off, because it is an account takeover waiting to happen: with it on, whoever the
         * provider calls "admin" becomes euclid's admin, and euclid has no say in who that is.
         * An installation whose provider *is* the authority on its user names - which is the
         * point of federating, for many - turns it on and its existing accounts keep working; one
         * that leaves it off links accounts by having an administrator set oidcSubject, or lets
         * new users be provisioned alongside the old ones.
         */
        bool linkExistingUsers{false};

        /**
         * @brief PEM CA certificate trusted in addition to the system trust store, for a provider
         * behind a private CA. Empty uses the system trust store alone.
         */
        std::string caCertFile;

        /**
         * @brief How long an authorization may stay in flight - the lifetime sealed into the state
         * parameter, after which a callback carrying it is refused as stale.
         */
        std::chrono::seconds stateTtl{std::chrono::minutes(10)};

        /**
         * @brief URL prefixes a browser-driven login may be sent on to when it succeeds.
         *
         * @par
         * Empty by default, which allows only a path on the gateway itself. This is not a
         * formality: a successful browser login ends in a redirect carrying the session token in
         * the URL fragment, so an unrestricted "return to" is a link an attacker can send a person
         * that hands their token to whoever wrote the link. A front end served from somewhere else
         * - the web console, typically - is named here, and nothing else is accepted.
         */
        std::vector<std::string> returnToPrefixes;

        /**
         * @brief Whether a browser may be sent on to @p returnTo after signing in.
         *
         * @param returnTo the URL a caller asked to be returned to; empty is always allowed and
         * means the login response is answered directly.
         * @return true if the redirect is one this installation permits.
         */
        [[nodiscard]]
        bool IsReturnToAllowed(const std::string &returnTo) const;

        /**
         * @brief Reads the configuration of one module's OIDC block.
         *
         * @param module module whose block to read, e.g. "eam".
         * @return the configuration, with enabled=false if the block is absent.
         */
        static OidcConfiguration FromConfiguration(const std::string &module);

        /**
         * @brief What is missing or wrong, one message per problem.
         *
         * Separated from reading so a module can report every problem at startup rather than
         * failing on the first one at the first login attempt.
         *
         * @return an empty vector when the configuration is usable.
         */
        [[nodiscard]]
        std::vector<std::string> Validate() const;
    };

    /**
     * @brief Raised for every way an OIDC exchange can fail: an unreachable provider, a refused
     * code, a token that does not verify.
     *
     * @par
     * One type rather than several because every caller does the same thing with them - refuses
     * the login and logs the reason. The message says which of them happened.
     */
    struct OidcError final : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief The transient half of an authorization, sealed into the "state" parameter.
     *
     * @par
     * A flow that starts on one request and finishes on another has to carry its nonce and PKCE
     * verifier across the gap. Keeping them in memory would be wrong here for a specific reason:
     * eam runs as a pool (see euclid.modules.eam.maxInstances), so the callback is routinely
     * answered by a different process than the one that issued the redirect, and an in-memory map
     * would refuse those logins at random. Putting them in the database would work, but it means a
     * schema, a TTL sweep and two round trips per login for something that lives ten minutes.
     *
     * @par
     * So the state is the storage: this struct, encrypted with AES-256-GCM under a key derived
     * from the installation's JWT secret, base64url-encoded. Any instance can open it because they
     * share the secret; nobody else can read it or forge one, because they do not.
     */
    struct OidcState {

        /**
         * @brief Value the ID token's "nonce" claim has to match, tying the token to this flow.
         */
        std::string nonce;

        /**
         * @brief PKCE code verifier, presented at the token endpoint to redeem the code.
         */
        std::string codeVerifier;

        /**
         * @brief Redirect URI this flow started with. Sent again at the token endpoint, which
         * requires the two to be identical, and not taken from the callback request - a redirect
         * URI a caller can restate at will is one it can also change.
         */
        std::string redirectUri;

        /**
         * @brief Where to send the browser once login succeeds, for a browser-driven flow. Empty
         * for a caller (the CLI, a server-side client) that wants the login response itself.
         */
        std::string returnTo;

        /**
         * @brief When this state stops being accepted, as a Unix timestamp.
         */
        long expiresAt{0};

        /**
         * @brief Encrypts and encodes this state for use as the "state" query parameter.
         *
         * @param secret the installation's JWT secret; the AES key is derived from it.
         * @return base64url of IV || ciphertext || tag.
         */
        [[nodiscard]]
        std::string Seal(const std::string &secret) const;

        /**
         * @brief Recovers a state produced by Seal().
         *
         * @param sealed the "state" parameter as it came back from the provider.
         * @param secret the same secret Seal() was given.
         * @return the state, or std::nullopt if it was not sealed by this installation, was
         * tampered with, or has expired.
         */
        static std::optional<OidcState> Open(const std::string &sealed, const std::string &secret);
    };

    /**
     * @brief Verifies an ID token against a JWKS, with no I/O of its own.
     *
     * @par
     * Split out from OidcClient precisely because it touches nothing: given a token and the keys
     * it should be signed with, whether it is acceptable is a pure question, and one worth being
     * able to ask in a test with a locally minted key instead of against a live provider.
     */
    class OidcTokenVerifier {

    public:

        /**
         * @brief Checks signature, issuer, audience, expiry and nonce, and extracts the identity.
         *
         * @param idToken the ID token as received from the token endpoint.
         * @param jwksJson the provider's JWKS document.
         * @param config the issuer and client ID to require, and which claims to read.
         * @param expectedNonce the nonce this flow sent; the token's must equal it.
         * @param error set to why verification failed, untouched on success.
         * @return the identity, or std::nullopt if the token is not acceptable.
         */
        [[nodiscard]]
        static std::optional<FederatedIdentity> Verify(const std::string &idToken, const std::string &jwksJson,
                                                  const OidcConfiguration &config, const std::string &expectedNonce,
                                                  std::string &error);
    };

    /**
     * @brief Drives the authorization-code flow with PKCE against an OIDC provider.
     *
     * @par
     * Euclid is the relying party here, not the issuer: the provider authenticates the person, and
     * what comes back is turned into an ordinary euclid session by the caller (see the eam
     * module's oidc-login). Nothing downstream of login learns that a login was federated - the
     * token, the access key and the grants are the same ones a password login produces.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class OidcClient {

    public:

        /**
         * @brief Where to send the browser, and the state that goes with it.
         */
        struct Authorization {

            /**
             * @brief The provider's authorization endpoint with every parameter already on it.
             */
            std::string url;

            /**
             * @brief The sealed state, also present inside url - returned separately so a caller
             * that has to hand the callback back to euclid itself (the CLI) can hold on to it.
             */
            std::string state;
        };

        /**
         * @brief Constructs the client.
         *
         * @param config the provider configuration.
         * @param stateSecret secret the state parameter is sealed with, i.e. the installation's
         * JWT secret.
         */
        OidcClient(OidcConfiguration config, std::string stateSecret);

        /**
         * @brief Starts a flow: generates the nonce and PKCE pair, seals them into the state and
         * builds the authorization URL.
         *
         * @param redirectUri where the provider should send the browser, or empty for the
         * configured one.
         * @param returnTo where the caller wants the browser to end up afterwards, or empty for a
         * caller that will collect the login response itself.
         * @return the authorization URL and its state.
         * @throws OidcError if the provider cannot be discovered.
         */
        [[nodiscard]]
        Authorization BeginAuthorization(const std::string &redirectUri, const std::string &returnTo) const;

        /**
         * @brief Finishes a flow: redeems the code and verifies the ID token that comes back.
         *
         * @param code the authorization code from the callback.
         * @param state the state from the callback, as sealed by BeginAuthorization().
         * @param openedState set to the recovered state, so a caller can honour its returnTo.
         * @return the verified identity.
         * @throws OidcError if the state is unusable, the provider refuses the code, or the ID
         * token does not verify.
         */
        [[nodiscard]]
        FederatedIdentity Complete(const std::string &code, const std::string &state, OidcState &openedState) const;

        /**
         * @brief The configuration this client was built with.
         */
        [[nodiscard]]
        const OidcConfiguration &Config() const { return _config; }

    private:

        /**
         * @brief The three endpoints, from the configuration where it names them and from the
         * issuer's discovery document otherwise.
         *
         * @throws OidcError if discovery is needed and fails.
         */
        [[nodiscard]]
        OidcConfiguration Discovered() const;

        OidcConfiguration _config;
        std::string _stateSecret;
    };

}// namespace Euclid::Core
