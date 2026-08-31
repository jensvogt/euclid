//
// Created by vogje01 on 9/1/26.
//

// C++ includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

// OpenSSL includes
#include <openssl/crypto.h>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/HttpSignature.h>

namespace Euclid::Core {

    namespace {

        namespace http = boost::beast::http;

        std::string trim(const std::string &s) {
            const auto begin = s.find_first_not_of(" \t");
            if (begin == std::string::npos) return {};
            const auto end = s.find_last_not_of(" \t");
            return s.substr(begin, end - begin + 1);
        }

        std::string toLower(std::string s) {
            std::ranges::transform(s, s.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // Same reasoning as SigV4's comparison: the result of a signature check must not leak
        // through how long it took to fail.
        bool constantTimeEquals(const std::string &a, const std::string &b) {
            if (a.size() != b.size()) return false;
            return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
        }

        // CryptoUtils hashes to hex; Content-Digest carries the raw digest in base64, so the hex
        // is turned back into bytes here rather than adding a second hashing entry point.
        std::string fromHex(const std::string &hex) {
            std::string out;
            out.reserve(hex.size() / 2);
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            }
            return out;
        }

        // A field value as the signature base wants it: leading/trailing whitespace stripped. The
        // requests euclid signs never carry a field more than once, so the multi-value joining
        // RFC 9421 §2.1 describes has nothing to do here.
        std::optional<std::string> fieldValue(const http::request<http::string_body> &req, const std::string &name) {
            const auto value = req[name];
            if (value.empty()) return std::nullopt;
            return trim(std::string(value));
        }

        // RFC 9421 §2.2 derived components. Only the three euclid's requests can meaningfully
        // cover are implemented: every request goes to the same path with no query string, so
        // @query/@request-target would add nothing a signature could protect.
        std::optional<std::string> derivedValue(const http::request<http::string_body> &req, const std::string &name) {

            if (name == "@method") return std::string(req.method_string());

            if (name == "@path") {
                std::string target(req.target());
                const auto query = target.find('?');
                return query == std::string::npos ? target : target.substr(0, query);
            }

            if (name == "@authority") {
                const auto host = req[http::field::host];
                if (host.empty()) return std::nullopt;
                // Lowercased per RFC 9421: authority is case-insensitive, so signer and verifier
                // have to agree on one spelling.
                return toLower(trim(std::string(host)));
            }

            return std::nullopt;
        }

        long nowSeconds() {
            return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }

        // Reads the value of a `;name=value` parameter out of a serialized parameter list.
        // Quotes around a string value are stripped; an absent parameter yields an empty string.
        std::string parameterValue(const std::string &parameters, const std::string &name) {

            const auto needle = ";" + name + "=";
            const auto pos = parameters.find(needle);
            if (pos == std::string::npos) return {};

            auto start = pos + needle.size();
            if (start >= parameters.size()) return {};

            if (parameters[start] == '"') {
                const auto end = parameters.find('"', start + 1);
                if (end == std::string::npos) return {};
                return parameters.substr(start + 1, end - start - 1);
            }

            const auto end = parameters.find(';', start);
            return end == std::string::npos ? parameters.substr(start) : parameters.substr(start, end - start);
        }

        long parameterNumber(const std::string &parameters, const std::string &name) {
            const auto value = parameterValue(parameters, name);
            if (value.empty()) return 0;
            try {
                return std::stol(value);
            } catch (const std::exception &) {
                return 0;
            }
        }

    }// namespace

    const std::vector<std::string> &HttpSignature::CoveredComponents() {
        static const std::vector<std::string> kComponents = {
                "@method", "@path", "@authority", "content-digest",
                "x-euclid-account-id", "x-euclid-action", "x-euclid-region", "x-euclid-target", "x-euclid-user-id"};
        return kComponents;
    }

    std::optional<HttpSignature::ParsedSignatureInput> HttpSignature::ParseSignatureInput(const std::string &headerValue) {

        const auto equals = headerValue.find('=');
        if (equals == std::string::npos) return std::nullopt;

        ParsedSignatureInput parsed;
        parsed.label = trim(headerValue.substr(0, equals));
        parsed.parameters = trim(headerValue.substr(equals + 1));
        if (parsed.label.empty() || parsed.parameters.empty()) return std::nullopt;

        // The component list is the parenthesized inner list the parameters open with.
        if (!parsed.parameters.starts_with('(')) return std::nullopt;
        const auto close = parsed.parameters.find(')');
        if (close == std::string::npos) return std::nullopt;

        const auto list = parsed.parameters.substr(1, close - 1);
        std::istringstream stream(list);
        std::string item;
        while (stream >> item) {
            // Every component is a quoted string; anything else (a parameterized component such
            // as "@query-param";name="x") is not something this accepts.
            if (item.size() < 2 || item.front() != '"' || item.back() != '"') return std::nullopt;
            parsed.components.push_back(item.substr(1, item.size() - 2));
        }
        if (parsed.components.empty()) return std::nullopt;

        const auto parameters = parsed.parameters.substr(close + 1);
        parsed.keyId = parameterValue(parameters, "keyid");
        parsed.algorithm = parameterValue(parameters, "alg");
        parsed.created = parameterNumber(parameters, "created");
        parsed.expires = parameterNumber(parameters, "expires");

        return parsed;
    }

    std::string HttpSignature::ContentDigest(const std::string &body) {
        return "sha-256=:" + CryptoUtils::Base64Encode(fromHex(CryptoUtils::sha256Hex(body))) + ":";
    }

    std::optional<std::string> HttpSignature::BuildSignatureBase(const http::request<http::string_body> &req,
                                                                 const std::vector<std::string> &components, const std::string &parameters) {

        std::string base;
        for (const auto &component: components) {
            const auto value = component.starts_with('@') ? derivedValue(req, component) : fieldValue(req, component);
            // A covered component that isn't in the request cannot be verified, and silently
            // treating it as empty would let a signature cover a header that was then removed.
            if (!value.has_value()) return std::nullopt;
            base += "\"" + component + "\": " + *value + "\n";
        }
        base += "\"@signature-params\": " + parameters;
        return base;
    }

    void HttpSignature::Sign(http::request<http::string_body> &req, const std::string &accessKeyId, const std::string &secretAccessKey) {

        req.set("Content-Digest", ContentDigest(req.body()));

        std::string components;
        for (std::size_t i = 0; i < CoveredComponents().size(); ++i) {
            if (i > 0) components += ' ';
            components += "\"" + CoveredComponents()[i] + "\"";
        }
        const std::string parameters = "(" + components + ");created=" + std::to_string(nowSeconds()) +
                                       ";keyid=\"" + accessKeyId + "\";alg=\"" + std::string(Algorithm) + "\"";

        const auto base = BuildSignatureBase(req, CoveredComponents(), parameters);
        if (!base.has_value()) return;

        const auto signature = CryptoUtils::hmacSha256({secretAccessKey.begin(), secretAccessKey.end()}, *base);

        req.set("Signature-Input", std::string(Label) + "=" + parameters);
        req.set("Signature", std::string(Label) + "=:" + CryptoUtils::Base64Encode({signature.begin(), signature.end()}) + ":");
    }

    bool HttpSignature::IsSigned(const http::request<http::string_body> &req) {
        return !req["Signature-Input"].empty() && !req["Signature"].empty();
    }

    std::optional<HttpSignature::VerifyResult> HttpSignature::Verify(const http::request<http::string_body> &req,
                                                                     const std::function<std::optional<std::string>(const std::string &)> &lookupSecret,
                                                                     const std::chrono::seconds maxSkew) {

        const auto parsed = ParseSignatureInput(std::string(req["Signature-Input"]));
        if (!parsed.has_value()) return std::nullopt;

        // Fixed policy, not client-negotiated - see the class comment for why the covered set
        // can't be whatever the signer felt like covering.
        if (parsed->components != CoveredComponents()) return std::nullopt;
        if (parsed->algorithm != Algorithm) return std::nullopt;
        if (parsed->keyId.empty()) return std::nullopt;

        const auto now = nowSeconds();
        if (parsed->created == 0) return std::nullopt;
        if (const auto skew = now - parsed->created; skew > maxSkew.count() || skew < -maxSkew.count()) return std::nullopt;
        if (parsed->expires != 0 && parsed->expires < now) return std::nullopt;

        // The body is covered only through this header, so it has to be checked against the body
        // actually received before the signature over it means anything.
        const auto digest = req["Content-Digest"];
        if (digest.empty()) return std::nullopt;
        if (!constantTimeEquals(std::string(digest), ContentDigest(req.body()))) return std::nullopt;

        const auto secret = lookupSecret(parsed->keyId);
        if (!secret.has_value()) return std::nullopt;

        const auto base = BuildSignatureBase(req, parsed->components, parsed->parameters);
        if (!base.has_value()) return std::nullopt;

        const auto expected = CryptoUtils::hmacSha256({secret->begin(), secret->end()}, *base);
        const auto expectedHeader = parsed->label + "=:" + CryptoUtils::Base64Encode({expected.begin(), expected.end()}) + ":";

        if (!constantTimeEquals(trim(std::string(req["Signature"])), expectedHeader)) return std::nullopt;

        return VerifyResult{.accessKeyId = parsed->keyId};
    }

}// namespace Euclid::Core
