//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Boost includes
#include <boost/beast/http.hpp>

namespace Euclid::Core {

    /**
     * @brief AWS Signature Version 4 request signing and verification.
     *
     * Adapted to euclid's own request shape: unlike real AWS, the target service/action live in
     * custom "x-euclid-*" headers rather than the URI, so those headers are always part of the
     * signed set - a fixed, non-negotiable list rather than a client-chosen "SignedHeaders" list,
     * so a MITM can't downgrade which headers a signature actually covers.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class SigV4 {
    public:

        /**
         * @brief Headers that are always part of the signature, in the (already alphabetical)
         * order the canonical request requires.
         *
         * host and x-amz-content-sha256/x-amz-date give the usual SigV4 transport/payload
         * integrity; the x-euclid-* headers are signed too because that's where euclid carries
         * routing information AWS would normally put in the URI.
         */
        static const std::vector<std::string> &SignedHeaderNames();

        /**
         * @brief The "<accessKeyId>/<date>/<region>/<service>/aws4_request" scope parsed out of
         * an Authorization header.
         */
        struct CredentialScope {
            std::string accessKeyId;
            std::string dateStamp;// YYYYMMDD
            std::string region;
            std::string service;
        };

        /**
         * @brief Authorization header, split into its components.
         */
        struct ParsedAuthorization {
            CredentialScope scope;
            std::string signedHeaders;// as literally presented, semicolon-joined
            std::string signature;    // lowercase hex
        };

        /**
         * @brief Parses "AWS4-HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=...".
         *
         * @param headerValue raw value of the Authorization header.
         * @return the parsed components, or std::nullopt if the header isn't present/well-formed.
         */
        [[nodiscard]]
        static std::optional<ParsedAuthorization> ParseAuthorizationHeader(const std::string &headerValue);

        /**
         * @brief Canonicalizes a raw query string: percent-decodes each name/value, re-encodes
         * per SigV4's URI-encoding rules, and sorts by name then value.
         *
         * Exposed standalone (rather than folded into Sign/Verify) so it can be tested directly
         * against AWS's published SigV4 test vectors.
         *
         * @param rawQuery the query string as it appears on the wire (without the leading '?'),
         *                 e.g. "Param2=value2&Param1=value1". Empty if the request has none.
         * @return the canonical query string, e.g. "Param1=value1&Param2=value2".
         */
        [[nodiscard]]
        static std::string CanonicalizeQueryString(const std::string &rawQuery);

        /**
         * @brief Builds the SigV4 canonical request string.
         *
         * Exposed as a standalone step (rather than folded into Sign/Verify) so it can be tested
         * directly against AWS's published SigV4 test vectors.
         *
         * @param method            HTTP method, e.g. "POST".
         * @param canonicalUri      URI-encoded path, e.g. "/".
         * @param canonicalQuery    canonical query string (sorted, encoded key=value&...), empty if none.
         * @param headers           all request headers, keyed by lowercase name.
         * @param signedHeaderNames headers to include, already sorted (see SignedHeaderNames()).
         * @param payloadHashHex    lowercase hex SHA-256 of the request body.
         * @return the canonical request string.
         */
        [[nodiscard]]
        static std::string BuildCanonicalRequest(const std::string &method, const std::string &canonicalUri, const std::string &canonicalQuery,
                                                  const std::map<std::string, std::string> &headers, const std::vector<std::string> &signedHeaderNames,
                                                  const std::string &payloadHashHex);

        /**
         * @brief Builds the SigV4 string-to-sign.
         *
         * @param amzDate                full request timestamp, e.g. "20260818T120000Z".
         * @param credentialScope         "<date>/<region>/<service>/aws4_request".
         * @param canonicalRequestHashHex lowercase hex SHA-256 of the canonical request.
         * @return the string-to-sign.
         */
        [[nodiscard]]
        static std::string BuildStringToSign(const std::string &amzDate, const std::string &credentialScope, const std::string &canonicalRequestHashHex);

        /**
         * @brief Derives the SigV4 signing key via the kDate -> kRegion -> kService -> kSigning
         * HMAC-SHA256 chain.
         *
         * @param secretAccessKey the caller's secret.
         * @param dateStamp       "YYYYMMDD".
         * @param region          e.g. "eu-central-1".
         * @param service         e.g. "sqs".
         * @return the derived signing key (raw bytes).
         */
        [[nodiscard]]
        static std::vector<unsigned char> DeriveSigningKey(const std::string &secretAccessKey, const std::string &dateStamp, const std::string &region, const std::string &service);

        /**
         * @brief Signs a request in place: sets x-amz-date, x-amz-content-sha256 and Authorization.
         *
         * Call after every other header the signature must cover (x-euclid-target,
         * x-euclid-action, x-euclid-region, x-euclid-account-id, x-euclid-user-id, host) and the
         * body are already set on req.
         *
         * @param req               request to sign; mutated in place.
         * @param accessKeyId       the caller's access key ID.
         * @param secretAccessKey   the caller's secret access key.
         * @param region            region to scope the signature to, e.g. "eu-central-1".
         * @param service           service to scope the signature to, e.g. "sqs".
         */
        static void Sign(boost::beast::http::request<boost::beast::http::string_body> &req, const std::string &accessKeyId,
                          const std::string &secretAccessKey, const std::string &region, const std::string &service);

        /**
         * @brief Result of a successful Verify() call.
         */
        struct VerifyResult {
            std::string accessKeyId;
        };

        /**
         * @brief Verifies a SigV4-signed request.
         *
         * Recomputes the signature from the request exactly as received and compares it
         * (constant-time) against the one presented in Authorization - any change to the signed
         * headers or body between signing and here makes this fail.
         *
         * @param req           the request to verify.
         * @param lookupSecret  resolves an access key ID to its secret, or std::nullopt if unknown.
         * @param maxSkew       maximum age (in either direction) x-amz-date may have relative to
         *                      now before the request is rejected as stale/replayed.
         * @return the resolved access key ID on success, std::nullopt on any failure (missing/
         *         malformed header, unknown key, stale timestamp, or signature mismatch) - callers
         *         don't get to distinguish which, mirroring how JWT verification collapses failure
         *         modes into a single rejection.
         */
        [[nodiscard]]
        static std::optional<VerifyResult> Verify(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                                    const std::function<std::optional<std::string>(const std::string &)> &lookupSecret,
                                                    std::chrono::seconds maxSkew = std::chrono::minutes(15));
    };

}// namespace Euclid::Core
