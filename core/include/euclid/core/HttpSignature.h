//
// Created by vogje01 on 9/1/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Boost includes
#include <boost/beast/http.hpp>

namespace Euclid::Core {

    /**
     * @brief RFC 9421 HTTP Message Signatures, as an alternative to SigV4.
     *
     * @par
     * Same job as Core::SigV4 and the same credentials - an access key ID names the key, its
     * secret access key is the HMAC key - but the standard, non-AWS way of saying it: what the
     * signature covers is listed explicitly in a Signature-Input header, the signature itself is
     * base64 in a Signature header, and the body is bound in through a Content-Digest header
     * (RFC 9530) rather than through a hash header only AWS clients know to send. A client that
     * already speaks HTTP Message Signatures needs no euclid-specific signing code.
     *
     * @par Fixed policy
     * Like SigV4 here, the covered component list is not client-negotiated: a signature must
     * cover exactly CoveredComponents(), in that order, or it is rejected. RFC 9421 leaves the
     * choice to the signer, which would let a MITM strip a component from both the signature
     * base and the header it is presented in and still verify - the same downgrade the SigV4
     * implementation next door refuses.
     *
     * @par Algorithm
     * hmac-sha256 only. The asymmetric algorithms RFC 9421 also defines need a public-key
     * distribution story euclid does not have; the access key secret both sides already share is
     * exactly an HMAC key.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class HttpSignature {

    public:

        /**
         * @brief The only signature algorithm accepted, as it appears in the "alg" parameter.
         */
        static constexpr std::string_view Algorithm = "hmac-sha256";

        /**
         * @brief Label the signature is presented under, i.e. the "sig1" in
         * "Signature-Input: sig1=(...)".
         */
        static constexpr std::string_view Label = "sig1";

        /**
         * @brief Components every signature must cover, in the order they appear in the
         * signature base.
         *
         * @par
         * "@method", "@path" and "@authority" are RFC 9421 derived components, taken from the
         * request line and the Host header. "content-digest" binds the body. The x-euclid-*
         * headers are covered for the same reason SigV4 signs them: that is where euclid carries
         * the routing information a REST API would put in the URI.
         */
        static const std::vector<std::string> &CoveredComponents();

        /**
         * @brief A Signature-Input header value, split into its parts.
         */
        struct ParsedSignatureInput {

            /**
             * @brief Signature label, e.g. "sig1".
             */
            std::string label;

            /**
             * @brief Covered component names, in the order presented.
             */
            std::vector<std::string> components;

            /**
             * @brief Access key ID naming the HMAC key, from the "keyid" parameter.
             */
            std::string keyId;

            /**
             * @brief Value of the "alg" parameter; empty if the signer omitted it.
             */
            std::string algorithm;

            /**
             * @brief Value of the "created" parameter, as a Unix timestamp.
             */
            long created{};

            /**
             * @brief Value of the "expires" parameter, as a Unix timestamp; 0 if absent.
             */
            long expires{};

            /**
             * @brief Everything after "<label>=", exactly as presented.
             *
             * The signature base's last line has to repeat the signature parameters
             * byte-for-byte as the signer serialized them, so this is kept verbatim rather than
             * rebuilt from the fields above - a re-serialization that differed anywhere (spacing,
             * parameter order) would compute a different base and fail every signature.
             */
            std::string parameters;
        };

        /**
         * @brief Parses a Signature-Input header value.
         *
         * @param headerValue raw header value, e.g. "sig1=(\"@method\");created=1;keyid=\"k\"".
         * @return the parsed input, or std::nullopt if it is missing or malformed.
         */
        [[nodiscard]]
        static std::optional<ParsedSignatureInput> ParseSignatureInput(const std::string &headerValue);

        /**
         * @brief Renders a body as a Content-Digest header value, e.g. "sha-256=:MV9b23...:".
         *
         * @param body request body; may be empty, which still has a digest.
         * @return the header value.
         */
        [[nodiscard]]
        static std::string ContentDigest(const std::string &body);

        /**
         * @brief Builds the signature base: one line per covered component, then the
         * "@signature-params" line.
         *
         * Exposed as its own step (rather than folded into Sign/Verify) so it can be checked
         * against the worked examples in RFC 9421.
         *
         * @param req        request the component values are taken from.
         * @param components covered components, in order.
         * @param parameters the signature parameters exactly as they appear in Signature-Input.
         * @return the signature base, or std::nullopt if a covered component is missing from req.
         */
        [[nodiscard]]
        static std::optional<std::string> BuildSignatureBase(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                                             const std::vector<std::string> &components, const std::string &parameters);

        /**
         * @brief Signs a request in place: sets Content-Digest, Signature-Input and Signature.
         *
         * Call after every header the signature must cover (x-euclid-target, x-euclid-action,
         * x-euclid-region, x-euclid-account-id, x-euclid-user-id, host) and the body are already
         * set on req.
         *
         * @param req             request to sign; mutated in place.
         * @param accessKeyId     the caller's access key ID, sent as the "keyid" parameter.
         * @param secretAccessKey the caller's secret access key, used as the HMAC key.
         */
        static void Sign(boost::beast::http::request<boost::beast::http::string_body> &req, const std::string &accessKeyId, const std::string &secretAccessKey);

        /**
         * @brief Result of a successful Verify() call.
         */
        struct VerifyResult {
            std::string accessKeyId;
        };

        /**
         * @brief Whether a request presents an RFC 9421 signature at all.
         *
         * Lets a caller that accepts several schemes decide which verifier to run without
         * treating "no signature headers" as a verification failure.
         *
         * @param req request to inspect.
         * @return true if both Signature-Input and Signature are present.
         */
        [[nodiscard]]
        static bool IsSigned(const boost::beast::http::request<boost::beast::http::string_body> &req);

        /**
         * @brief Verifies an RFC 9421-signed request.
         *
         * Recomputes the signature from the request exactly as received and compares it
         * (constant-time) against the one presented - any change to a covered component or to the
         * body between signing and here makes this fail.
         *
         * @param req          the request to verify.
         * @param lookupSecret resolves an access key ID to its secret, or std::nullopt if unknown.
         * @param maxSkew      how far "created" may sit from now, in either direction, before the
         *                     request is rejected as stale or replayed.
         * @return the resolved access key ID on success, std::nullopt on any failure (missing or
         *         malformed headers, unexpected component list, unknown key, stale timestamp,
         *         body/digest mismatch, or signature mismatch) - callers don't get to distinguish
         *         which, matching how SigV4 and JWT verification collapse failures into one
         *         rejection.
         */
        [[nodiscard]]
        static std::optional<VerifyResult> Verify(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                                  const std::function<std::optional<std::string>(const std::string &)> &lookupSecret,
                                                  std::chrono::seconds maxSkew = std::chrono::minutes(15));
    };

}// namespace Euclid::Core
