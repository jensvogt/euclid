// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <chrono>

// Boost includes
#include <boost/json.hpp>

// jwt-cpp includes
#include <jwt-cpp/traits/boost-json/traits.h>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/Federation.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/OidcClient.h>
#include <euclid/core/TtlCache.h>
#include <euclid/core/WebClient.h>

namespace Euclid::Core {

    namespace {

        // Discovery documents and key sets change rarely, and a login should not pay for fetching
        // both every time. Short enough that a key rotation is picked up on its own; an unknown
        // "kid" also forces a refresh before the token is refused (see fetchJwks()).
        constexpr auto kMetadataTtl = std::chrono::minutes(5);

        // PKCE, RFC 7636: 32 random bytes is comfortably inside the 43-128 character range the
        // spec allows once base64url-encoded.
        constexpr std::size_t kCodeVerifierBytes = 32;
        constexpr std::size_t kNonceBytes = 16;

        std::string percentEncode(const std::string_view value) {
            static constexpr std::string_view unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
            std::string encoded;
            encoded.reserve(value.size());
            for (const char c: value) {
                if (unreserved.find(c) != std::string_view::npos) {
                    encoded += c;
                } else {
                    static constexpr char hex[] = "0123456789ABCDEF";
                    encoded += '%';
                    encoded += hex[static_cast<unsigned char>(c) >> 4];
                    encoded += hex[static_cast<unsigned char>(c) & 0x0F];
                }
            }
            return encoded;
        }

        std::string formEncode(const std::vector<std::pair<std::string, std::string> > &parameters) {
            std::string encoded;
            for (const auto &[name, value]: parameters) {
                if (!encoded.empty()) encoded += "&";
                encoded += percentEncode(name) + "=" + percentEncode(value);
            }
            return encoded;
        }

        // Reads a string claim, returning empty rather than throwing when it is absent or is not a
        // string - every claim read here is optional as far as this code is concerned, and a
        // provider that sends a number where a name belongs should not crash a login.
        std::string stringClaim(const boost::json::value &payload, const std::string &name) {
            if (!payload.is_object()) return {};
            const auto *claim = payload.as_object().if_contains(name);
            if (claim == nullptr || !claim->is_string()) return {};
            return std::string(claim->as_string());
        }

        boost::json::value fetchJson(const std::string &url, const std::string &caCertFile) {

            WebResponse result;
            try {
                result = WebFetch(url, {.caCertFile = caCertFile});
            } catch (const WebError &e) {
                throw OidcError(e.what());
            }
            if (!result.IsSuccess()) {
                throw OidcError("GET " + url + " answered HTTP " + std::to_string(result.status));
            }

            boost::system::error_code ec;
            auto parsed = boost::json::parse(result.body, ec);
            if (ec) throw OidcError("GET " + url + " did not answer JSON: " + ec.message());
            return parsed;
        }

        // The discovery document, cached. Keyed by URL rather than held in one variable because a
        // process may well talk to more than one issuer over its life (a configuration change does
        // not restart it).
        std::string discoveryDocument(const std::string &issuer, const std::string &caCertFile) {

            static TtlCache<std::string, std::string> cache{std::chrono::duration_cast<std::chrono::milliseconds>(kMetadataTtl)};

            std::string url = issuer;
            while (!url.empty() && url.back() == '/') url.pop_back();
            url += "/.well-known/openid-configuration";

            const auto document = cache.get(url, [&caCertFile](const std::string &key) -> std::optional<std::string> {
                log_debug << "Fetching OIDC discovery document, url: " << key;
                return boost::json::serialize(fetchJson(key, caCertFile));
            });
            if (!document.has_value()) throw OidcError("Could not read the OIDC discovery document at " + url);
            return *document;
        }

        TtlCache<std::string, std::string> &jwksCache() {
            static TtlCache<std::string, std::string> cache{std::chrono::duration_cast<std::chrono::milliseconds>(kMetadataTtl)};
            return cache;
        }

        std::string fetchJwks(const std::string &jwksUri, const std::string &caCertFile) {
            const auto keys = jwksCache().get(jwksUri, [&caCertFile](const std::string &key) -> std::optional<std::string> {
                log_debug << "Fetching OIDC key set, url: " << key;
                return boost::json::serialize(fetchJson(key, caCertFile));
            });
            if (!keys.has_value()) throw OidcError("Could not read the OIDC key set at " + jwksUri);
            return *keys;
        }

    }// namespace

    // ── Configuration ────────────────────────────────────────────────────────

    OidcConfiguration OidcConfiguration::FromConfiguration(const std::string &module) {

        const auto &cfg = Configuration::instance();
        const std::string prefix = "euclid.modules." + module + ".oidc.";

        OidcConfiguration config;
        config.enabled = cfg.getOr<bool>(prefix + "enabled", false);
        config.issuer = cfg.getOr<std::string>(prefix + "issuer", "");
        config.clientId = cfg.getOr<std::string>(prefix + "client-id", "");
        config.clientSecret = cfg.getOr<std::string>(prefix + "client-secret", "");
        config.redirectUri = cfg.getOr<std::string>(prefix + "redirect-uri", "");
        config.scopes = cfg.getOr<std::string>(prefix + "scopes", "openid profile email");
        config.authorizationEndpoint = cfg.getOr<std::string>(prefix + "authorization-endpoint", "");
        config.tokenEndpoint = cfg.getOr<std::string>(prefix + "token-endpoint", "");
        config.jwksUri = cfg.getOr<std::string>(prefix + "jwks-uri", "");
        config.usernameClaim = cfg.getOr<std::string>(prefix + "username-claim", "preferred_username");
        config.emailClaim = cfg.getOr<std::string>(prefix + "email-claim", "email");
        config.accountId = cfg.getOr<std::string>(prefix + "account-id", "");
        config.jitProvisioning = cfg.getOr<bool>(prefix + "jit-provisioning", true);
        config.linkExistingUsers = cfg.getOr<bool>(prefix + "link-existing-users", false);
        config.caCertFile = cfg.getOr<std::string>(prefix + "ca-cert-file", "");
        config.stateTtl = std::chrono::seconds(cfg.getOr<long>(prefix + "state-ttl-seconds", 600));

        if (cfg.has(prefix + "return-to-prefixes")) {
            config.returnToPrefixes = cfg.getArray<std::string>(prefix + "return-to-prefixes");
        }

        // Where a just-in-time provisioned user lands when the block does not say. The same
        // account the bootstrap admin is created in, so a fresh installation federates into the
        // account it already has rather than into one nobody has granted anything on.
        if (config.accountId.empty() && cfg.has("euclid.account-ids")) {
            if (const auto accountIds = cfg.getArray<std::string>("euclid.account-ids"); !accountIds.empty()) {
                config.accountId = accountIds.front();
            }
        }

        // "openid" is what makes this OpenID Connect rather than plain OAuth: without it a
        // provider is entitled to return no ID token, and there is then nothing to verify an
        // identity from.
        if (config.scopes.find("openid") == std::string::npos) {
            config.scopes = config.scopes.empty() ? "openid" : "openid " + config.scopes;
        }
        return config;
    }

    bool OidcConfiguration::IsReturnToAllowed(const std::string &returnTo) const {
        return Core::IsReturnToAllowed(returnTo, returnToPrefixes);
    }

    std::vector<std::string> OidcConfiguration::Validate() const {

        std::vector<std::string> problems;
        if (!enabled) return problems;

        if (issuer.empty()) problems.emplace_back("oidc.issuer is not set");
        if (clientId.empty()) problems.emplace_back("oidc.client-id is not set");
        if (clientSecret.empty()) problems.emplace_back("oidc.client-secret is not set");

        // Only the issuer is required to be reachable for discovery; an installation that pins its
        // endpoints has to pin all three, since a half-pinned set still needs discovery and would
        // then quietly ignore what it was given.
        const bool anyEndpoint = !authorizationEndpoint.empty() || !tokenEndpoint.empty() || !jwksUri.empty();
        const bool allEndpoints = !authorizationEndpoint.empty() && !tokenEndpoint.empty() && !jwksUri.empty();
        if (anyEndpoint && !allEndpoints) {
            problems.emplace_back("oidc.authorization-endpoint, oidc.token-endpoint and oidc.jwks-uri have to be set together, or none of them");
        }

        if (!issuer.empty() && !issuer.starts_with("https://") && !issuer.starts_with("http://")) {
            problems.emplace_back("oidc.issuer has to be an absolute URL, e.g. https://example.onelogin.com/oidc/2");
        }
        return problems;
    }

    // ── State ────────────────────────────────────────────────────────────────

    std::string OidcState::Seal(const std::string &secret) const {

        return SealFederationState({
                                           {"nonce", nonce},
                                           {"verifier", codeVerifier},
                                           {"redirectUri", redirectUri},
                                           {"returnTo", returnTo},
                                   },
                                   secret, std::chrono::seconds(expiresAt - std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    }

    std::optional<OidcState> OidcState::Open(const std::string &sealed, const std::string &secret) {

        const auto payload = OpenFederationState(sealed, secret);
        if (!payload.has_value()) return std::nullopt;

        const boost::json::value asValue = *payload;
        OidcState state;
        state.nonce = GetStringValue(asValue, "nonce");
        state.codeVerifier = GetStringValue(asValue, "verifier");
        state.redirectUri = GetStringValue(asValue, "redirectUri");
        state.returnTo = GetStringValue(asValue, "returnTo");
        state.expiresAt = GetLongValue(asValue, "expiresAt");
        return state;
    }

    // ── Token verification ───────────────────────────────────────────────────

    std::optional<FederatedIdentity> OidcTokenVerifier::Verify(const std::string &idToken, const std::string &jwksJson,
                                                          const OidcConfiguration &config, const std::string &expectedNonce,
                                                          std::string &error) {

        try {
            const auto decoded = jwt::decode<jwt::traits::boost_json>(idToken);

            if (!decoded.has_key_id()) {
                error = "ID token carries no key id";
                return std::nullopt;
            }

            const auto keys = jwt::parse_jwks<jwt::traits::boost_json>(jwksJson);
            const auto key = keys.get_jwk(decoded.get_key_id());

            // A key set states its keys either as a certificate chain or as bare RSA components,
            // and providers differ on which - OneLogin publishes n/e. Both end up as the same PEM.
            std::string publicKey;
            if (key.has_jwk_claim("x5c")) {
                publicKey = jwt::helper::convert_base64_der_to_pem(key.get_x5c_key_value());
            } else {
                publicKey = jwt::helper::create_public_key_from_rsa_components(
                        key.get_jwk_claim("n").as_string(), key.get_jwk_claim("e").as_string());
            }

            // RS256 only, and named explicitly: a verifier that accepted whatever the token's own
            // header asked for would accept "none", and one that allowed HS256 would let a token
            // signed with the (public) key material verify.
            std::error_code ec;
            jwt::verify<jwt::traits::boost_json>()
                    .allow_algorithm(jwt::algorithm::rs256(publicKey))
                    .with_issuer(config.issuer)
                    .with_audience(config.clientId)
                    .leeway(60)
                    .verify(decoded, ec);
            if (ec) {
                error = "ID token did not verify: " + ec.message();
                return std::nullopt;
            }

            const auto payload = boost::json::parse(decoded.get_payload());

            // The nonce is what ties this token to the authorization this installation started.
            // Checked here rather than left to the caller because a token that verifies but
            // belongs to somebody else's flow is exactly the case worth refusing.
            if (const auto nonce = stringClaim(payload, "nonce"); nonce != expectedNonce) {
                error = "ID token nonce does not match the authorization request";
                return std::nullopt;
            }

            FederatedIdentity identity;
            identity.provider = "oidc";
            identity.subject = stringClaim(payload, "sub");
            if (identity.subject.empty()) {
                error = "ID token carries no subject";
                return std::nullopt;
            }
            identity.issuer = stringClaim(payload, "iss");
            identity.email = stringClaim(payload, config.emailClaim);

            // Falls back the way an operator would expect to read it: the claim that was asked
            // for, then the recognisable half of an email address, and only then the provider's
            // opaque subject - which is always there but which nobody can recognise.
            std::string userId = stringClaim(payload, config.usernameClaim);
            if (userId.empty() && !identity.email.empty()) userId = identity.email.substr(0, identity.email.find('@'));
            if (userId.empty()) userId = identity.subject;
            identity.userId = SanitizeUserId(userId);

            return identity;

        } catch (const std::exception &e) {
            error = std::string("ID token could not be read: ") + e.what();
            return std::nullopt;
        }
    }

    // ── Client ───────────────────────────────────────────────────────────────

    OidcClient::OidcClient(OidcConfiguration config, std::string stateSecret)
        : _config(std::move(config)), _stateSecret(std::move(stateSecret)) {}

    OidcConfiguration OidcClient::Discovered() const {

        if (!_config.authorizationEndpoint.empty() && !_config.tokenEndpoint.empty() && !_config.jwksUri.empty()) {
            return _config;
        }

        const auto document = boost::json::parse(discoveryDocument(_config.issuer, _config.caCertFile));

        OidcConfiguration discovered = _config;
        if (discovered.authorizationEndpoint.empty()) discovered.authorizationEndpoint = GetStringValue(document, "authorization_endpoint");
        if (discovered.tokenEndpoint.empty()) discovered.tokenEndpoint = GetStringValue(document, "token_endpoint");
        if (discovered.jwksUri.empty()) discovered.jwksUri = GetStringValue(document, "jwks_uri");

        // The issuer the document states wins over the one that was configured, so that a value
        // written with or without a trailing slash still matches the "iss" claim later on.
        if (const auto issuer = GetStringValue(document, "issuer"); !issuer.empty()) discovered.issuer = issuer;

        if (discovered.authorizationEndpoint.empty() || discovered.tokenEndpoint.empty() || discovered.jwksUri.empty()) {
            throw OidcError("The discovery document of " + _config.issuer + " does not name all of authorization_endpoint, token_endpoint and jwks_uri");
        }
        return discovered;
    }

    OidcClient::Authorization OidcClient::BeginAuthorization(const std::string &redirectUri, const std::string &returnTo) const {

        const auto config = Discovered();

        const std::string effectiveRedirectUri = redirectUri.empty() ? config.redirectUri : redirectUri;
        if (effectiveRedirectUri.empty()) {
            throw OidcError("No redirect URI: neither the request nor oidc.redirect-uri names one");
        }

        OidcState state;
        state.nonce = CryptoUtils::Base64UrlEncode(CryptoUtils::GenerateSalt(kNonceBytes));
        state.codeVerifier = CryptoUtils::Base64UrlEncode(CryptoUtils::GenerateSalt(kCodeVerifierBytes));
        state.redirectUri = effectiveRedirectUri;
        state.returnTo = returnTo;
        state.expiresAt = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + config.stateTtl).time_since_epoch()).count();

        const auto sealed = state.Seal(_stateSecret);
        const auto challenge = CryptoUtils::Base64UrlEncode(CryptoUtils::sha256Raw(state.codeVerifier));

        std::string url = config.authorizationEndpoint;
        url += url.find('?') == std::string::npos ? "?" : "&";
        url += formEncode({
                {"response_type", "code"},
                {"client_id", config.clientId},
                {"redirect_uri", effectiveRedirectUri},
                {"scope", config.scopes},
                {"state", sealed},
                {"nonce", state.nonce},
                {"code_challenge", challenge},
                {"code_challenge_method", "S256"},
        });

        return {.url = url, .state = sealed};
    }

    FederatedIdentity OidcClient::Complete(const std::string &code, const std::string &state, OidcState &openedState) const {

        if (code.empty()) throw OidcError("No authorization code in the callback");

        const auto opened = OidcState::Open(state, _stateSecret);
        if (!opened.has_value()) {
            throw OidcError("The callback's state is not one this installation issued, or it has expired");
        }
        openedState = *opened;

        const auto config = Discovered();

        std::vector<std::pair<std::string, std::string> > form{
                {"grant_type", "authorization_code"},
                {"code", code},
                {"redirect_uri", opened->redirectUri},
                {"code_verifier", opened->codeVerifier},
        };

        // client_secret_basic where there is a secret, which is what a confidential client
        // registered with OneLogin gets by default; client_id in the body otherwise, which is the
        // only thing a public client can do.
        std::string authorization;
        if (config.clientSecret.empty()) {
            form.emplace_back("client_id", config.clientId);
        } else {
            authorization = "Basic " + CryptoUtils::Base64Encode(percentEncode(config.clientId) + ":" + percentEncode(config.clientSecret));
        }

        WebResponse result;
        try {
            result = WebFetch(config.tokenEndpoint, {.method = "POST",
                                                            .body = formEncode(form),
                                                            .contentType = "application/x-www-form-urlencoded",
                                                            .authorization = authorization,
                                                            .caCertFile = config.caCertFile});
        } catch (const WebError &e) {
            throw OidcError(e.what());
        }

        boost::system::error_code parseEc;
        const auto answer = boost::json::parse(result.body, parseEc);
        if (parseEc) throw OidcError("The token endpoint did not answer JSON (HTTP " + std::to_string(result.status) + ")");

        if (!result.IsSuccess()) {
            const auto error = GetStringValue(answer, "error");
            const auto description = GetStringValue(answer, "error_description");
            throw OidcError("The token endpoint refused the authorization code: " +
                            (error.empty() ? "HTTP " + std::to_string(result.status) : error) +
                            (description.empty() ? "" : " (" + description + ")"));
        }

        const auto idToken = GetStringValue(answer, "id_token");
        if (idToken.empty()) throw OidcError("The token endpoint returned no ID token");

        std::string error;
        auto identity = OidcTokenVerifier::Verify(idToken, fetchJwks(config.jwksUri, config.caCertFile), config, opened->nonce, error);

        // One retry against a freshly fetched key set, for the one failure that is not the token's
        // fault: a provider that has rotated its keys since the cached copy was taken. Refusing a
        // valid login for up to the cache TTL after every rotation is not an acceptable answer.
        if (!identity.has_value()) {
            log_debug << "ID token did not verify against the cached key set, refetching, error: " << error;
            jwksCache().clear();
            std::string retryError;
            identity = OidcTokenVerifier::Verify(idToken, fetchJwks(config.jwksUri, config.caCertFile), config, opened->nonce, retryError);
            if (!identity.has_value()) throw OidcError(retryError);
        }

        return *identity;
    }

}// namespace Euclid::Core
