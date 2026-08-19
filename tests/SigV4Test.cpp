#define BOOST_TEST_MODULE SigV4Test
#include <boost/test/unit_test.hpp>

// C++ includes
#include <iomanip>
#include <optional>
#include <sstream>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/SigV4.h>

// Test vectors taken verbatim from AWS's official published SigV4 test suite
// (github.com/aws/aws-sig-v4-test-suite, credentials/date/region/service shared by all cases:
// access key "AKIDEXAMPLE", secret "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
// 20150830T123600Z, us-east-1, service "service") - independent of this implementation, so a bug
// shared between Sign() and Verify() can't make these agree with each other while both being
// wrong relative to the real algorithm.

using Euclid::Core::CryptoUtils;
using Euclid::Core::SigV4;

namespace {
    constexpr auto kSecretAccessKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    constexpr auto kDateStamp = "20150830";
    constexpr auto kAmzDate = "20150830T123600Z";
    constexpr auto kRegion = "us-east-1";
    constexpr auto kService = "service";
    constexpr auto kCredentialScope = "20150830/us-east-1/service/aws4_request";

    std::string signatureFor(const std::string &canonicalRequest) {
        const auto stringToSign = SigV4::BuildStringToSign(kAmzDate, kCredentialScope, CryptoUtils::sha256Hex(canonicalRequest));
        const auto signingKey = SigV4::DeriveSigningKey(kSecretAccessKey, kDateStamp, kRegion, kService);
        const auto mac = CryptoUtils::hmacSha256(signingKey, stringToSign);

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (const unsigned char b: mac) oss << std::setw(2) << static_cast<int>(b);
        return oss.str();
    }
}

// get-vanilla: GET /, no query, only host/x-amz-date signed, empty body.
BOOST_AUTO_TEST_CASE (GetVanilla) {

    const std::map<std::string, std::string> headers = {
        {"host", "example.amazonaws.com"},
        {"x-amz-date", kAmzDate},
    };
    const std::vector<std::string> signedHeaders = {"host", "x-amz-date"};
    const auto payloadHash = CryptoUtils::sha256Hex("");

    const auto canonicalRequest = SigV4::BuildCanonicalRequest("GET", "/", "", headers, signedHeaders, payloadHash);

    BOOST_TEST(canonicalRequest ==
               "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n"
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const auto stringToSign = SigV4::BuildStringToSign(kAmzDate, kCredentialScope, CryptoUtils::sha256Hex(canonicalRequest));
    BOOST_TEST(stringToSign ==
               "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
               "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63");

    BOOST_TEST(signatureFor(canonicalRequest) == "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
}

// get-vanilla-query-order-key-case: query parameters must be sorted alphabetically by key,
// regardless of the order they appear in the request.
BOOST_AUTO_TEST_CASE (GetVanillaQueryOrderKeyCase) {

    const std::map<std::string, std::string> headers = {
        {"host", "example.amazonaws.com"},
        {"x-amz-date", kAmzDate},
    };
    const std::vector<std::string> signedHeaders = {"host", "x-amz-date"};
    const auto payloadHash = CryptoUtils::sha256Hex("");

    const auto canonicalQuery = SigV4::CanonicalizeQueryString("Param2=value2&Param1=value1");
    BOOST_TEST(canonicalQuery == "Param1=value1&Param2=value2");

    const auto canonicalRequest = SigV4::BuildCanonicalRequest("GET", "/", canonicalQuery, headers, signedHeaders, payloadHash);

    BOOST_TEST(canonicalRequest ==
               "GET\n/\nParam1=value1&Param2=value2\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\n"
               "host;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    BOOST_TEST(signatureFor(canonicalRequest) == "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500");
}

// post-x-www-form-urlencoded: non-empty body and multiple signed headers, including one
// (content-length) that isn't part of euclid's own fixed signed-header set - BuildCanonicalRequest
// takes the header list as a parameter precisely so it isn't tied to that fixed set.
BOOST_AUTO_TEST_CASE (PostXWwwFormUrlencoded) {

    const std::string body = "Param1=value1";
    BOOST_TEST(CryptoUtils::sha256Hex(body) == "9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e");

    const std::map<std::string, std::string> headers = {
        {"content-length", "13"},
        {"content-type", "application/x-www-form-urlencoded"},
        {"host", "example.amazonaws.com"},
        {"x-amz-date", kAmzDate},
    };
    const std::vector<std::string> signedHeaders = {"content-length", "content-type", "host", "x-amz-date"};

    const auto canonicalRequest = SigV4::BuildCanonicalRequest("POST", "/", "", headers, signedHeaders, CryptoUtils::sha256Hex(body));

    BOOST_TEST(canonicalRequest ==
               "POST\n/\n\ncontent-length:13\ncontent-type:application/x-www-form-urlencoded\n"
               "host:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\n"
               "content-length;content-type;host;x-amz-date\n"
               "9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e");

    BOOST_TEST(signatureFor(canonicalRequest) == "fec50118d90ecf934441dd37fb9a49bd7f5adb6450802ca3a0977623bbb7c27f");
}

// Sign() followed by Verify() over the same request must round-trip, and a request tampered with
// after signing (either the body or a signed header) must be rejected - this is the actual
// property the rest of the system relies on for MITM tamper detection.
BOOST_AUTO_TEST_CASE (SignThenVerifyRoundTrips) {
    namespace http = boost::beast::http;

    const auto buildRequest = [] {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::host, "example.amazonaws.com");
        req.set("x-euclid-target", "queues");
        req.set("x-euclid-action", "send-message");
        req.set("x-euclid-region", "eu-central-1");
        req.set("x-euclid-account-id", "863459426936");
        req.set("x-euclid-user-id", "alice");
        req.body() = R"({"queueUrl":"http://localhost/q","messageBody":"hello"})";
        req.prepare_payload();
        return req;
    };

    const std::string accessKeyId = "AKIDEXAMPLE";
    const std::string secretAccessKey = kSecretAccessKey;

    auto lookup = [&](const std::string &id) -> std::optional<std::string> {
        return id == accessKeyId ? std::optional(secretAccessKey) : std::nullopt;
    };

    auto signed_ = buildRequest();
    SigV4::Sign(signed_, accessKeyId, secretAccessKey, kRegion, "queues");

    const auto verified = SigV4::Verify(signed_, lookup);
    BOOST_TEST_REQUIRE(verified.has_value());
    BOOST_TEST(verified->accessKeyId == accessKeyId);

    // Tampered body must invalidate the signature.
    auto tamperedBody = signed_;
    tamperedBody.body() = R"({"queueUrl":"http://localhost/q","messageBody":"pwned"})";
    tamperedBody.prepare_payload();
    BOOST_TEST(!SigV4::Verify(tamperedBody, lookup).has_value());

    // Tampered routing header (a MITM re-targeting the request) must invalidate the signature too.
    auto tamperedTarget = signed_;
    tamperedTarget.set("x-euclid-target", "s3");
    BOOST_TEST(!SigV4::Verify(tamperedTarget, lookup).has_value());

    // An unknown access key ID must be rejected.
    auto unknownKey = signed_;
    SigV4::Sign(unknownKey, "AKIDNOTFOUND", secretAccessKey, kRegion, "queues");
    BOOST_TEST(!SigV4::Verify(unknownKey, lookup).has_value());
}
