//
// Created by vogje01 on 9/9/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::Core {

    /**
     * @brief Who an identity provider says somebody is, once euclid has verified that it really
     * said it.
     *
     * @par
     * One type for both federations. What OIDC learns from a signed ID token and what SAML learns
     * from a signed assertion is the same handful of facts, and everything downstream - matching a
     * euclid user, provisioning one, issuing the session - should not be able to tell them apart,
     * or the two would drift into behaving differently for no reason anybody chose.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct FederatedIdentity {

        /**
         * @brief Which federation this came through: "oidc" or "saml".
         *
         * @par
         * Part of what a user is matched on, not decoration: the two namespaces of subjects are
         * unrelated, and a SAML NameID that happens to read like an OIDC subject must not find
         * that user.
         */
        std::string provider;

        /**
         * @brief The provider's own, permanent identifier - an OIDC "sub" claim or a SAML NameID.
         * What a euclid user is matched on.
         */
        std::string subject;

        /**
         * @brief The euclid user ID this identity maps to, already sanitised.
         */
        std::string userId;

        /**
         * @brief Email address, empty if the provider released none.
         */
        std::string email;

        /**
         * @brief The issuer that vouched for this identity, as verified.
         */
        std::string issuer;

        /**
         * @brief SAML only: the ID of the assertion this came from, so the same one cannot be
         * presented twice. Empty for OIDC, where the authorization code is single-use at the
         * provider and there is nothing here to replay.
         */
        std::string assertionId;

        /**
         * @brief SAML only: when the assertion stops being valid, and therefore how long its ID
         * has to be remembered.
         */
        std::chrono::system_clock::time_point assertionExpiresAt{};
    };

    /**
     * @brief Encrypts a small payload for round-tripping through a browser.
     *
     * @par
     * Both federations have to carry a few values across the gap between sending somebody to their
     * provider and hearing back - a nonce and PKCE verifier for OIDC, a request ID for SAML - and
     * neither can keep them in memory: eam runs as a pool, so the return leg is routinely answered
     * by a different process than the one that sent them away. This makes the round-trip parameter
     * itself the storage: AES-256-GCM under a key derived from the installation's JWT secret, so
     * any instance can open it and nobody else can read or forge one.
     *
     * @param payload what to carry; an "expiresAt" member is added.
     * @param secret the installation's JWT secret.
     * @param ttl how long the result stays acceptable.
     * @return base64url of IV || ciphertext || tag.
     */
    std::string SealFederationState(const boost::json::object &payload, const std::string &secret, std::chrono::seconds ttl);

    /**
     * @brief Recovers a payload sealed by SealFederationState().
     *
     * @param sealed the parameter as it came back.
     * @param secret the same secret it was sealed with.
     * @return the payload, or std::nullopt if it was not sealed by this installation, was altered,
     * or has expired.
     */
    std::optional<boost::json::object> OpenFederationState(const std::string &sealed, const std::string &secret);

    /**
     * @brief Whether a browser may be sent on to @p returnTo once it has signed in.
     *
     * @par
     * Not a formality: a browser login ends in a redirect that carries the session, so an
     * unrestricted destination is a link an attacker can send somebody that hands over their
     * token. A path on the gateway itself is always allowed, a loopback address is allowed (that
     * is the CLI catching its own login, and a redirect there cannot leave the machine), and
     * anything else has to be named in @p prefixes.
     *
     * @param returnTo the destination asked for; empty is allowed and means "answer me directly".
     * @param prefixes destinations this installation permits, from configuration.
     * @return true if the redirect is permitted.
     */
    bool IsReturnToAllowed(const std::string &returnTo, const std::vector<std::string> &prefixes);

    /**
     * @brief Whether a URL points at this machine, and so at something the person signing in is
     * already running - the CLI's listener.
     *
     * @param url the URL to examine.
     * @return true for http://127.0.0.1, http://[::1] or http://localhost, with any port.
     */
    bool IsLoopbackUrl(const std::string &url);

    /**
     * @brief Narrows whatever a provider released to something that can safely be a euclid user ID.
     *
     * @par
     * A user ID is not just a label - it goes into an ERN, which is colon-separated, and into
     * access-key ownership. Only ':' and whitespace strictly matter, but the rule is written the
     * other way round, as a whitelist, so that a claim nobody anticipated cannot smuggle anything
     * through.
     *
     * @param claim the name the provider gave.
     * @return the same name with anything unacceptable replaced by '-'.
     */
    std::string SanitizeUserId(const std::string &claim);

}// namespace Euclid::Core
