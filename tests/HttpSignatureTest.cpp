#define BOOST_TEST_MODULE HttpSignatureTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/HttpSignature.h>

using Euclid::Core::CryptoUtils;
using Euclid::Core::HttpSignature;

namespace http = boost::beast::http;

// RFC 9421 signing and verification, the alternative to SigV4 for the same access key. What
// matters here is that a signature covers what it claims to cover: change any covered component,
// the body, or the key, and verification has to fail.

namespace {

    constexpr auto kAccessKeyId = "AKIAEXAMPLE0000000001";
    constexpr auto kSecret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    http::request<http::string_body> buildRequest(const std::string &body = R"({"bucketErn":"ern:esm:x"})") {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::host, "localhost:5566");
        req.set("x-euclid-target", "esm");
        req.set("x-euclid-action", "list-objects");
        req.set("x-euclid-region", "eu-central-1");
        req.set("x-euclid-account-id", "000000000000");
        req.set("x-euclid-user-id", "admin");
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
        return req;
    }

    // Stands in for the access-key repository the modules wire in.
    auto keyStore(const std::string &keyId = kAccessKeyId, const std::string &secret = kSecret) {
        return [keyId, secret](const std::string &requested) -> std::optional<std::string> {
            if (requested != keyId) return std::nullopt;
            return secret;
        };
    }

}// namespace

BOOST_AUTO_TEST_CASE(SignedRequestVerifies) {
    auto req = buildRequest();
    HttpSignature::Sign(req, kAccessKeyId, kSecret);

    // The three headers RFC 9421 (and RFC 9530, for the digest) puts on the wire.
    BOOST_TEST(!req["Signature-Input"].empty());
    BOOST_TEST(!req["Signature"].empty());
    BOOST_TEST(req["Content-Digest"].starts_with("sha-256=:"));
    BOOST_TEST(req["Signature"].starts_with("sig1=:"));
    BOOST_TEST(HttpSignature::IsSigned(req));

    const auto result = HttpSignature::Verify(req, keyStore());
    BOOST_TEST_REQUIRE(result.has_value());
    BOOST_TEST(result->accessKeyId == kAccessKeyId);
}

BOOST_AUTO_TEST_CASE(AnUnsignedRequestIsNotSigned) {
    const auto req = buildRequest();
    BOOST_TEST(!HttpSignature::IsSigned(req));
    BOOST_TEST(!HttpSignature::Verify(req, keyStore()).has_value());
}

BOOST_AUTO_TEST_CASE(TamperingWithACoveredHeaderFails) {
    auto req = buildRequest();
    HttpSignature::Sign(req, kAccessKeyId, kSecret);

    // Re-routing the call to another module is exactly what covering x-euclid-target prevents.
    req.set("x-euclid-target", "eqs");
    BOOST_TEST(!HttpSignature::Verify(req, keyStore()).has_value());
}

BOOST_AUTO_TEST_CASE(TamperingWithTheBodyFails) {
    auto req = buildRequest();
    HttpSignature::Sign(req, kAccessKeyId, kSecret);

    req.body() = R"({"bucketErn":"ern:esm:someone-elses-bucket"})";
    req.prepare_payload();

    // Caught by the Content-Digest check before the signature is even recomputed - the digest is
    // the only thing binding the body to the signature.
    BOOST_TEST(!HttpSignature::Verify(req, keyStore()).has_value());
}

BOOST_AUTO_TEST_CASE(AStrippedDigestFails) {
    auto req = buildRequest();
    HttpSignature::Sign(req, kAccessKeyId, kSecret);
    req.erase("Content-Digest");

    BOOST_TEST(!HttpSignature::Verify(req, keyStore()).has_value());
}

BOOST_AUTO_TEST_CASE(TheWrongKeyFails) {
    auto req = buildRequest();
    HttpSignature::Sign(req, kAccessKeyId, kSecret);

    // Right key ID, wrong secret.
    BOOST_TEST(!HttpSignature::Verify(req, keyStore(kAccessKeyId, "not-the-secret")).has_value());
    // Unknown key ID.
    BOOST_TEST(!HttpSignature::Verify(req, keyStore("AKIAUNKNOWN", kSecret)).has_value());
}

BOOST_AUTO_TEST_CASE(ANarrowerCoveredSetIsRefused) {
    // The downgrade the fixed component list exists to stop: a signature that legitimately covers
    // only @method still verifies as a signature, but must not be accepted here, or an attacker
    // could strip every other component from both the base and the request.
    auto req = buildRequest();
    const std::string parameters = R"(("@method");created=)" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()) +
                                   R"(;keyid=")" + kAccessKeyId + R"(";alg="hmac-sha256")";

    req.set("Content-Digest", HttpSignature::ContentDigest(req.body()));
    const auto base = HttpSignature::BuildSignatureBase(req, {"@method"}, parameters);
    BOOST_TEST_REQUIRE(base.has_value());

    const auto signature = CryptoUtils::hmacSha256({std::string(kSecret).begin(), std::string(kSecret).end()}, *base);
    req.set("Signature-Input", "sig1=" + parameters);
    req.set("Signature", "sig1=:" + CryptoUtils::Base64Encode({signature.begin(), signature.end()}) + ":");

    BOOST_TEST(!HttpSignature::Verify(req, keyStore()).has_value());
}

BOOST_AUTO_TEST_CASE(AStaleSignatureFails) {
    auto req = buildRequest();
    HttpSignature::Sign(req, kAccessKeyId, kSecret);

    // The signature itself is still valid; it is the age that is not. Anything replayed later
    // than the skew window is refused.
    BOOST_TEST(!HttpSignature::Verify(req, keyStore(), std::chrono::seconds(-1)).has_value());
}

BOOST_AUTO_TEST_CASE(SignatureInputParsesIntoItsParts) {
    const auto parsed = HttpSignature::ParseSignatureInput(
            R"(sig1=("@method" "@path" "content-digest");created=1735689600;keyid="AKIA1";alg="hmac-sha256";expires=1735693200)");

    BOOST_TEST_REQUIRE(parsed.has_value());
    BOOST_TEST(parsed->label == "sig1");
    BOOST_TEST_REQUIRE(parsed->components.size() == 3U);
    BOOST_TEST(parsed->components[0] == "@method");
    BOOST_TEST(parsed->components[2] == "content-digest");
    BOOST_TEST(parsed->keyId == "AKIA1");
    BOOST_TEST(parsed->algorithm == "hmac-sha256");
    BOOST_TEST(parsed->created == 1735689600L);
    BOOST_TEST(parsed->expires == 1735693200L);

    // The parameters are kept verbatim: the signature base repeats them byte for byte, so a
    // re-serialization that differed anywhere would break every signature.
    BOOST_TEST(parsed->parameters == R"(("@method" "@path" "content-digest");created=1735689600;keyid="AKIA1";alg="hmac-sha256";expires=1735693200)");

    BOOST_TEST(!HttpSignature::ParseSignatureInput("nonsense").has_value());
    BOOST_TEST(!HttpSignature::ParseSignatureInput("sig1=(unquoted);keyid=\"k\"").has_value());
}

BOOST_AUTO_TEST_CASE(SignatureBaseMatchesRfc9421Section26) {
    // RFC 9421 section 2.6's worked signature base, for the message in section 2.1. Checking the
    // construction against the specification's own bytes is what says this is HTTP Message
    // Signatures rather than a lookalike: line per component, quoted lowercase name, colon and a
    // single space, the "@signature-params" line last and unterminated.
    http::request<http::string_body> req(http::verb::post, "/foo?param=Value&Pet=dog", 11);
    req.set(http::field::host, "example.com");
    req.set("Date", "Tue, 20 Apr 2021 02:07:55 GMT");
    req.set("Content-Digest", "sha-512=:WZDPaVn/7XgHaAy8pmojAkGWoRx2UFChF41A2svX+T"
                              "aPm+AbwAgBWnrIiYllu7BNNyealdVLvRwEmTHWXvJwew==:");
    req.set("Content-Type", "application/json");
    req.set("Content-Length", "18");

    const std::string parameters = R"(("@method" "@authority" "@path" "content-digest" "content-length" "content-type");created=1618884473;keyid="test-key-rsa-pss")";
    const auto base = HttpSignature::BuildSignatureBase(
            req, {"@method", "@authority", "@path", "content-digest", "content-length", "content-type"}, parameters);

    BOOST_TEST_REQUIRE(base.has_value());
    BOOST_TEST(*base ==
               "\"@method\": POST\n"
               "\"@authority\": example.com\n"
               "\"@path\": /foo\n"
               "\"content-digest\": sha-512=:WZDPaVn/7XgHaAy8pmojAkGWoRx2UFChF41A2svX+TaPm+AbwAgBWnrIiYllu7BNNyealdVLvRwEmTHWXvJwew==:\n"
               "\"content-length\": 18\n"
               "\"content-type\": application/json\n"
               "\"@signature-params\": (\"@method\" \"@authority\" \"@path\" \"content-digest\" \"content-length\" \"content-type\");created=1618884473;keyid=\"test-key-rsa-pss\"");
}

BOOST_AUTO_TEST_CASE(InteroperatesWithAnIndependentImplementation) {
    // A request signed by examples/applications/python/euclid_app.py - a few dozen lines of
    // Python standard library, written against the RFC rather than against this code. Pinning its
    // output here is what says an application in another language can actually authenticate: the
    // signature base, the digest and the HMAC all have to agree byte for byte, and none of that
    // is checked by signing and verifying with the same implementation.
    //
    // Verified through BuildSignatureBase() rather than Verify(), because "created" is a fixed
    // timestamp in a recorded vector and Verify() rightly refuses anything that old.
    http::request<http::string_body> req(http::verb::post, "/", 11);
    req.set(http::field::host, "localhost:5566");
    req.set("x-euclid-account-id", "000000000000");
    req.set("x-euclid-action", "list-buckets");
    req.set("x-euclid-region", "eu-central-1");
    req.set("x-euclid-target", "esm");
    req.set("x-euclid-user-id", "appuser");
    req.set("Content-Digest", "sha-256=:0iKG6xLm1EmJVf31IlRq2NUIwWuh2GKuD8una+P3jh8=:");
    req.body() = R"({"pageSize":10})";
    req.prepare_payload();

    // The digest the Python side computed over that body.
    BOOST_TEST(HttpSignature::ContentDigest(req.body()) == "sha-256=:0iKG6xLm1EmJVf31IlRq2NUIwWuh2GKuD8una+P3jh8=:");

    const std::string parameters =
            R"(("@method" "@path" "@authority" "content-digest" "x-euclid-account-id" "x-euclid-action" "x-euclid-region" "x-euclid-target" "x-euclid-user-id");created=1788255252;keyid="AKIAEXAMPLE";alg="hmac-sha256")";

    const auto parsed = HttpSignature::ParseSignatureInput("sig1=" + parameters);
    BOOST_TEST_REQUIRE(parsed.has_value());
    BOOST_TEST(parsed->components == HttpSignature::CoveredComponents());

    const auto base = HttpSignature::BuildSignatureBase(req, parsed->components, parsed->parameters);
    BOOST_TEST_REQUIRE(base.has_value());

    const std::string secret = "topsecret";
    const auto signature = CryptoUtils::hmacSha256({secret.begin(), secret.end()}, *base);
    BOOST_TEST("sig1=:" + CryptoUtils::Base64Encode({signature.begin(), signature.end()}) + ":" ==
               "sig1=:RmC2h+H6ntZSgvXb6q5XbHqeRvTcBlyipElVGWViv2M=:");
}

BOOST_AUTO_TEST_CASE(ContentDigestIsTheRfc9530Form) {
    // RFC 9530's own example: the digest of {"hello": "world"} as a base64 SHA-256 in a byte
    // sequence.
    BOOST_TEST(HttpSignature::ContentDigest("{\"hello\": \"world\"}") ==
               "sha-256=:X48E9qOokqqrvdts8nOJRJN3OWDUoyWxBf7kbu9DBPE=:");

    // An empty body still has a digest, so a GET is covered the same way a POST is.
    BOOST_TEST(HttpSignature::ContentDigest("") == "sha-256=:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=:");
}
