// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE OidcTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// jwt-cpp includes
#include <jwt-cpp/traits/boost-json/traits.h>

// Euclid includes
#include "FederationTestKeys.h"
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/HttpUtils.h>
#include <euclid/core/OidcClient.h>

using Euclid::Core::CryptoUtils;
using Euclid::Core::OidcConfiguration;
using Euclid::Core::OidcState;
using Euclid::Core::OidcTokenVerifier;

// The half of the OIDC login that can be decided without a provider: what the state parameter
// protects, and which ID tokens are acceptable. The other half - discovery, the code exchange -
// is I/O against somebody else's server and is not what these test.
//
// The keys below are test fixtures, generated for this file and used nowhere else.

namespace {

    using Euclid::Test::kJwks;
    using Euclid::Test::kProviderKey;
    using Euclid::Test::kStrangerKey;

    constexpr auto kIssuer = "https://example.onelogin.com/oidc/2";
    constexpr auto kClientId = "euclid-test-client";
    constexpr auto kNonce = "nonce-of-this-flow";
    constexpr auto kSecret = "an-installation-jwt-secret-that-is-long-enough";

    OidcConfiguration configuration() {
        OidcConfiguration config;
        config.enabled = true;
        config.issuer = kIssuer;
        config.clientId = kClientId;
        config.clientSecret = "client-secret";
        config.usernameClaim = "preferred_username";
        config.emailClaim = "email";
        return config;
    }

    struct TokenOptions {
        std::string issuer{kIssuer};
        std::string audience{kClientId};
        std::string subject{"00u1abcd2efGhIjKl3m4"};
        std::string nonce{kNonce};
        std::string username{"jane.doe"};
        std::string email{"jane.doe@example.com"};
        std::string keyId{"test-key-1"};
        std::string signingKey{kProviderKey};
        std::chrono::seconds expiresIn{std::chrono::hours(1)};
        bool withUsername{true};
        bool withEmail{true};
    };

    std::string mintIdToken(const TokenOptions &options = {}) {

        const auto now = std::chrono::system_clock::now();
        auto token = jwt::create<jwt::traits::boost_json>()
                             .set_type("JWT")
                             .set_key_id(options.keyId)
                             .set_issuer(options.issuer)
                             .set_audience(options.audience)
                             .set_subject(options.subject)
                             .set_issued_at(now)
                             .set_expires_at(now + options.expiresIn)
                             .set_payload_claim("nonce", jwt::basic_claim<jwt::traits::boost_json>(options.nonce));

        if (options.withUsername) token.set_payload_claim("preferred_username", jwt::basic_claim<jwt::traits::boost_json>(options.username));
        if (options.withEmail) token.set_payload_claim("email", jwt::basic_claim<jwt::traits::boost_json>(options.email));

        return token.sign(jwt::algorithm::rs256("", options.signingKey, "", ""));
    }

    OidcState freshState() {
        OidcState state;
        state.nonce = kNonce;
        state.codeVerifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
        state.redirectUri = "http://127.0.0.1:41234/callback";
        state.returnTo = "https://console.example.com/signed-in";
        state.expiresAt = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::minutes(10)).time_since_epoch()).count();
        return state;
    }

}// namespace

// ── base64url and PKCE ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(base64url_round_trips_and_drops_padding) {

    const std::string data = "any bytes at all \x01\x02\xfe\xff, including ones that encode to + and /";

    const auto encoded = CryptoUtils::Base64UrlEncode(data);
    BOOST_TEST(encoded.find('=') == std::string::npos);
    BOOST_TEST(encoded.find('+') == std::string::npos);
    BOOST_TEST(encoded.find('/') == std::string::npos);
    BOOST_TEST(CryptoUtils::Base64UrlDecode(encoded) == data);
}

BOOST_AUTO_TEST_CASE(pkce_challenge_matches_the_rfc_7636_vector) {

    // RFC 7636 appendix B: the one worked example the spec gives, so a challenge this code
    // produces is one a provider will agree with.
    constexpr auto verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    constexpr auto expected = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";

    BOOST_TEST(CryptoUtils::Base64UrlEncode(CryptoUtils::sha256Raw(verifier)) == expected);
}

// ── the state parameter ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(state_survives_a_round_trip) {

    const auto state = freshState();
    const auto opened = OidcState::Open(state.Seal(kSecret), kSecret);

    BOOST_REQUIRE(opened.has_value());
    BOOST_TEST(opened->nonce == state.nonce);
    BOOST_TEST(opened->codeVerifier == state.codeVerifier);
    BOOST_TEST(opened->redirectUri == state.redirectUri);
    BOOST_TEST(opened->returnTo == state.returnTo);
}

BOOST_AUTO_TEST_CASE(state_hides_what_it_carries) {

    // The verifier is the secret half of PKCE. It travels through a browser's address bar, so it
    // has to be unreadable there - a state that merely encoded its contents would hand it over.
    const auto sealed = freshState().Seal(kSecret);

    BOOST_TEST(sealed.find("dBjftJeZ4CVP") == std::string::npos);
    BOOST_TEST(sealed.find(kNonce) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(state_from_another_installation_is_refused) {

    const auto sealed = freshState().Seal(kSecret);
    BOOST_TEST(!OidcState::Open(sealed, "a-different-installations-secret").has_value());
}

BOOST_AUTO_TEST_CASE(tampered_state_is_refused) {

    auto sealed = freshState().Seal(kSecret);

    // One character of ciphertext, changed to anything else. GCM authenticates what it encrypts,
    // so this cannot decrypt to something merely different - it cannot decrypt at all.
    sealed[sealed.size() / 2] = sealed[sealed.size() / 2] == 'A' ? 'B' : 'A';
    BOOST_TEST(!OidcState::Open(sealed, kSecret).has_value());
}

BOOST_AUTO_TEST_CASE(expired_state_is_refused) {

    auto state = freshState();
    state.expiresAt = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() - std::chrono::minutes(1)).time_since_epoch()).count();

    BOOST_TEST(!OidcState::Open(state.Seal(kSecret), kSecret).has_value());
}

BOOST_AUTO_TEST_CASE(garbage_state_is_refused) {

    BOOST_TEST(!OidcState::Open("not-a-state-at-all", kSecret).has_value());
    BOOST_TEST(!OidcState::Open("", kSecret).has_value());
}

// ── ID token verification ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_good_token_yields_the_identity) {

    std::string error;
    const auto identity = OidcTokenVerifier::Verify(mintIdToken(), kJwks, configuration(), kNonce, error);

    BOOST_REQUIRE_MESSAGE(identity.has_value(), error);
    BOOST_TEST(identity->subject == "00u1abcd2efGhIjKl3m4");
    BOOST_TEST(identity->userId == "jane.doe");
    BOOST_TEST(identity->email == "jane.doe@example.com");
    BOOST_TEST(identity->issuer == kIssuer);
}

BOOST_AUTO_TEST_CASE(a_token_signed_by_a_stranger_is_refused) {

    // Same key id, same claims, different key: this is the whole point of fetching a key set
    // rather than trusting what the token says about itself.
    TokenOptions options;
    options.signingKey = kStrangerKey;

    std::string error;
    BOOST_TEST(!OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error).has_value());
    BOOST_TEST(!error.empty());
}

BOOST_AUTO_TEST_CASE(a_token_from_another_issuer_is_refused) {

    TokenOptions options;
    options.issuer = "https://not-your.onelogin.com/oidc/2";

    std::string error;
    BOOST_TEST(!OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error).has_value());
}

BOOST_AUTO_TEST_CASE(a_token_for_another_client_is_refused) {

    // A provider serves many applications with one key. Without the audience check, a token
    // minted for any of the others would be accepted here.
    TokenOptions options;
    options.audience = "some-other-application";

    std::string error;
    BOOST_TEST(!OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error).has_value());
}

BOOST_AUTO_TEST_CASE(a_token_from_another_flow_is_refused) {

    TokenOptions options;
    options.nonce = "nonce-of-somebody-elses-login";

    std::string error;
    BOOST_TEST(!OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error).has_value());
}

BOOST_AUTO_TEST_CASE(an_expired_token_is_refused) {

    TokenOptions options;
    options.expiresIn = std::chrono::seconds(-3600);

    std::string error;
    BOOST_TEST(!OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error).has_value());
}

BOOST_AUTO_TEST_CASE(a_token_naming_an_unknown_key_is_refused) {

    TokenOptions options;
    options.keyId = "a-key-the-provider-does-not-publish";

    std::string error;
    BOOST_TEST(!OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error).has_value());
}

BOOST_AUTO_TEST_CASE(an_unsigned_token_is_refused) {

    // "alg": "none" - the oldest JWT attack there is, and one a verifier that trusted the token's
    // own header would fall for.
    const auto now = std::chrono::system_clock::now();
    const auto unsigned_ = jwt::create<jwt::traits::boost_json>()
                                   .set_type("JWT")
                                   .set_key_id("test-key-1")
                                   .set_issuer(kIssuer)
                                   .set_audience(kClientId)
                                   .set_subject("00u1abcd2efGhIjKl3m4")
                                   .set_issued_at(now)
                                   .set_expires_at(now + std::chrono::hours(1))
                                   .set_payload_claim("nonce", jwt::basic_claim<jwt::traits::boost_json>(std::string(kNonce)))
                                   .sign(jwt::algorithm::none{});

    std::string error;
    BOOST_TEST(!OidcTokenVerifier::Verify(unsigned_, kJwks, configuration(), kNonce, error).has_value());
}

BOOST_AUTO_TEST_CASE(the_user_id_falls_back_when_the_claim_is_absent) {

    // A provider that releases no preferred_username still has to produce a user ID an operator
    // can recognise, which is what the email local part is for.
    TokenOptions options;
    options.withUsername = false;

    std::string error;
    const auto identity = OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error);

    BOOST_REQUIRE_MESSAGE(identity.has_value(), error);
    BOOST_TEST(identity->userId == "jane.doe");
}

BOOST_AUTO_TEST_CASE(the_user_id_falls_back_to_the_subject) {

    TokenOptions options;
    options.withUsername = false;
    options.withEmail = false;

    std::string error;
    const auto identity = OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error);

    BOOST_REQUIRE_MESSAGE(identity.has_value(), error);
    BOOST_TEST(identity->userId == "00u1abcd2efGhIjKl3m4");
}

BOOST_AUTO_TEST_CASE(a_user_id_cannot_carry_ern_punctuation) {

    // A user ID ends up inside an ERN, which is colon-separated. A claim carrying colons (or
    // spaces, or anything else unexpected) must not be able to change how one parses.
    TokenOptions options;
    options.username = "ern:eam:x:y:user:root";

    std::string error;
    const auto identity = OidcTokenVerifier::Verify(mintIdToken(options), kJwks, configuration(), kNonce, error);

    BOOST_REQUIRE_MESSAGE(identity.has_value(), error);
    BOOST_TEST(identity->userId.find(':') == std::string::npos);
    BOOST_TEST(identity->userId == "ern-eam-x-y-user-root");
}

// ── the callback's query string ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(callback_parameters_are_read_and_decoded) {

    const auto parameters = Euclid::Core::ParseQueryParameters("/eam/oidc/callback?code=abc%2F123&state=x-y_z&scope=openid%20profile");

    BOOST_TEST(parameters.at("code") == "abc/123");
    BOOST_TEST(parameters.at("state") == "x-y_z");
    BOOST_TEST(parameters.at("scope") == "openid profile");
}

BOOST_AUTO_TEST_CASE(a_target_without_a_query_has_no_parameters) {

    BOOST_TEST(Euclid::Core::ParseQueryParameters("/eam/oidc/callback").empty());
    BOOST_TEST(Euclid::Core::ParseQueryParameters("").empty());
}

BOOST_AUTO_TEST_CASE(a_provider_refusal_is_readable) {

    const auto parameters = Euclid::Core::ParseQueryParameters("/eam/oidc/callback?error=access_denied&error_description=User%20cancelled");

    BOOST_TEST(parameters.at("error") == "access_denied");
    BOOST_TEST(parameters.at("error_description") == "User cancelled");
    BOOST_TEST(!parameters.contains("code"));
}

// ── configuration ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_disabled_configuration_has_nothing_to_complain_about) {

    const OidcConfiguration config;
    BOOST_TEST(config.Validate().empty());
}

BOOST_AUTO_TEST_CASE(an_enabled_configuration_names_what_is_missing) {

    OidcConfiguration config;
    config.enabled = true;

    const auto problems = config.Validate();
    BOOST_TEST(problems.size() == 3);
}

// ── where a browser may be sent afterwards ───────────────────────────────────

BOOST_AUTO_TEST_CASE(by_default_only_a_local_path_is_returned_to) {

    // The redirect at the end of a browser login carries the session token in its fragment, so an
    // unconfigured installation lets it go nowhere but back to itself.
    const auto config = configuration();

    BOOST_TEST(config.IsReturnToAllowed(""));
    BOOST_TEST(config.IsReturnToAllowed("/console/signed-in"));

    BOOST_TEST(!config.IsReturnToAllowed("https://evil.test/collect"));
    BOOST_TEST(!config.IsReturnToAllowed("http://evil.test/collect"));

    // A browser reads "//host/path" as another origin, whatever it looks like.
    BOOST_TEST(!config.IsReturnToAllowed("//evil.test/collect"));
}

BOOST_AUTO_TEST_CASE(a_configured_front_end_is_returned_to) {

    auto config = configuration();
    config.returnToPrefixes = {"https://console.example.com/"};

    BOOST_TEST(config.IsReturnToAllowed("https://console.example.com/"));
    BOOST_TEST(config.IsReturnToAllowed("https://console.example.com/signed-in?next=%2Fqueues"));

    // The lookalike host is the reason a prefix has to end at a path boundary.
    BOOST_TEST(!config.IsReturnToAllowed("https://console.example.com.evil.test/collect"));
    BOOST_TEST(!config.IsReturnToAllowed("https://another.example.com/signed-in"));
}

BOOST_AUTO_TEST_CASE(a_prefix_written_without_a_trailing_slash_still_ends_at_a_boundary) {

    auto config = configuration();
    config.returnToPrefixes = {"https://console.example.com"};

    BOOST_TEST(config.IsReturnToAllowed("https://console.example.com"));
    BOOST_TEST(config.IsReturnToAllowed("https://console.example.com/signed-in"));
    BOOST_TEST(!config.IsReturnToAllowed("https://console.example.com.evil.test/"));
}

BOOST_AUTO_TEST_CASE(pinned_endpoints_have_to_be_pinned_together) {

    auto config = configuration();
    config.tokenEndpoint = "https://example.onelogin.com/oidc/2/token";

    BOOST_TEST(!config.Validate().empty());

    config.authorizationEndpoint = "https://example.onelogin.com/oidc/2/auth";
    config.jwksUri = "https://example.onelogin.com/oidc/2/certs";
    BOOST_TEST(config.Validate().empty());
}
