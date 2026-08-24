// C++ includes
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

// OpenSSL includes
#include <openssl/crypto.h>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/SigV4.h>

namespace Euclid::Core {

    namespace {

        namespace http = boost::beast::http;

        constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";
        constexpr std::string_view kScopeTerminator = "aws4_request";

        std::string toHexLower(const std::vector<unsigned char> &bytes) {
            std::ostringstream oss;
            oss << std::hex << std::setfill('0');
            for (const unsigned char b: bytes) oss << std::setw(2) << static_cast<int>(b);
            return oss.str();
        }

        std::vector<unsigned char> toBytes(const std::string &s) {
            return {s.begin(), s.end()};
        }

        // Whether c is one of SigV4's unreserved characters (RFC 3986 unreserved set) - the only
        // characters left un-percent-encoded.
        bool isUnreserved(const unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        }

        // SigV4's URI-encoding: percent-encode everything except unreserved characters, with
        // uppercase hex digits. '/' is kept literal in the path (encodeSlash=false) but treated
        // like any other character in query keys/values (encodeSlash=true).
        std::string uriEncode(const std::string &value, const bool encodeSlash) {
            std::ostringstream oss;
            oss << std::hex << std::uppercase << std::setfill('0');
            for (const unsigned char c: value) {
                if (isUnreserved(c) || (c == '/' && !encodeSlash)) {
                    oss << static_cast<char>(c);
                } else {
                    oss << '%' << std::setw(2) << static_cast<int>(c);
                }
            }
            return oss.str();
        }

        std::string percentDecode(const std::string &value) {
            std::string out;
            out.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[i + 1])) && std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
                    out += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                } else {
                    out += value[i];
                }
            }
            return out;
        }

        std::string trim(const std::string &s) {
            const auto begin = s.find_first_not_of(" \t");
            if (begin == std::string::npos) return "";
            const auto end = s.find_last_not_of(" \t");
            return s.substr(begin, end - begin + 1);
        }

        std::vector<std::string> split(const std::string &s, const char delim) {
            std::vector<std::string> parts;
            std::size_t start = 0;
            while (start <= s.size()) {
                const auto pos = s.find(delim, start);
                if (pos == std::string::npos) {
                    parts.push_back(s.substr(start));
                    break;
                }
                parts.push_back(s.substr(start, pos - start));
                start = pos + 1;
            }
            return parts;
        }

        // Formats now as SigV4's full timestamp ("20260818T120000Z") and returns the date-only
        // prefix ("20260818") alongside it.
        std::pair<std::string, std::string> nowAmzDate() {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
            return {std::string(buf), std::string(buf, 8)};
        }

        // Parses a SigV4 timestamp ("20260818T120000Z") back to a time_point, for skew checking.
        std::optional<std::chrono::system_clock::time_point> parseAmzDate(const std::string &amzDate) {
            std::tm tm{};
            std::istringstream iss(amzDate);
            iss >> std::get_time(&tm, "%Y%m%dT%H%M%SZ");
            if (iss.fail()) return std::nullopt;
#if defined(_WIN32)
            const std::time_t t = _mkgmtime(&tm);
#else
            const std::time_t t = timegm(&tm);
#endif
            return std::chrono::system_clock::from_time_t(t);
        }

        std::map<std::string, std::string> headerMap(const http::request<http::string_body> &req) {
            std::map<std::string, std::string> headers;
            for (const auto &field: req) {
                std::string name(field.name_string());
                std::ranges::transform(name, name.begin(), [](const unsigned char c) { return std::tolower(c); });
                headers[name] = trim(std::string(field.value()));
            }
            return headers;
        }

        bool constantTimeEquals(const std::string &a, const std::string &b) {
            if (a.size() != b.size()) return false;
            return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
        }

    }// namespace

    std::string SigV4::CanonicalizeQueryString(const std::string &rawQuery) {
        if (rawQuery.empty()) return "";

        std::vector<std::pair<std::string, std::string>> params;
        for (const auto &pair: split(rawQuery, '&')) {
            if (pair.empty()) continue;
            const auto eq = pair.find('=');
            const std::string rawName = eq == std::string::npos ? pair : pair.substr(0, eq);
            const std::string rawValue = eq == std::string::npos ? "" : pair.substr(eq + 1);
            params.emplace_back(uriEncode(percentDecode(rawName), true), uriEncode(percentDecode(rawValue), true));
        }
        std::ranges::sort(params);

        std::string out;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i > 0) out += '&';
            out += params[i].first + "=" + params[i].second;
        }
        return out;
    }

    const std::vector<std::string> &SigV4::SignedHeaderNames() {
        static const std::vector<std::string> kNames = {
                "host", "x-amz-content-sha256", "x-amz-date",
                "x-euclid-account-id", "x-euclid-action", "x-euclid-region", "x-euclid-target", "x-euclid-user-id"};
        return kNames;
    }

    std::optional<SigV4::ParsedAuthorization> SigV4::ParseAuthorizationHeader(const std::string &headerValue) {

        if (!headerValue.starts_with(kAlgorithm)) return std::nullopt;

        std::string rest = trim(headerValue.substr(kAlgorithm.size()));
        std::string credential, signedHeaders, signature;

        for (const auto &part: split(rest, ',')) {
            const auto trimmed = trim(part);
            if (trimmed.starts_with("Credential=")) credential = trimmed.substr(std::strlen("Credential="));
            else if (trimmed.starts_with("SignedHeaders=")) signedHeaders = trimmed.substr(std::strlen("SignedHeaders="));
            else if (trimmed.starts_with("Signature=")) signature = trimmed.substr(std::strlen("Signature="));
        }
        if (credential.empty() || signedHeaders.empty() || signature.empty()) return std::nullopt;

        const auto scopeParts = split(credential, '/');
        if (scopeParts.size() != 5 || scopeParts[4] != kScopeTerminator) return std::nullopt;

        ParsedAuthorization parsed;
        parsed.scope = {.accessKeyId = scopeParts[0], .dateStamp = scopeParts[1], .region = scopeParts[2], .service = scopeParts[3]};
        parsed.signedHeaders = signedHeaders;
        parsed.signature = signature;
        return parsed;
    }

    std::string SigV4::BuildCanonicalRequest(const std::string &method, const std::string &canonicalUri, const std::string &canonicalQuery,
                                              const std::map<std::string, std::string> &headers, const std::vector<std::string> &signedHeaderNames,
                                              const std::string &payloadHashHex) {
        std::string canonicalHeaders;
        std::string signedHeadersStr;
        for (std::size_t i = 0; i < signedHeaderNames.size(); ++i) {
            const auto &name = signedHeaderNames[i];
            const auto it = headers.find(name);
            canonicalHeaders += name + ":" + (it != headers.end() ? it->second : "") + "\n";
            if (i > 0) signedHeadersStr += ";";
            signedHeadersStr += name;
        }

        return method + "\n" + canonicalUri + "\n" + canonicalQuery + "\n" + canonicalHeaders + "\n" + signedHeadersStr + "\n" + payloadHashHex;
    }

    std::string SigV4::BuildStringToSign(const std::string &amzDate, const std::string &credentialScope, const std::string &canonicalRequestHashHex) {
        return std::string(kAlgorithm) + "\n" + amzDate + "\n" + credentialScope + "\n" + canonicalRequestHashHex;
    }

    std::vector<unsigned char> SigV4::DeriveSigningKey(const std::string &secretAccessKey, const std::string &dateStamp, const std::string &region, const std::string &service) {
        const auto kDate = CryptoUtils::hmacSha256(toBytes("AWS4" + secretAccessKey), dateStamp);
        const auto kRegion = CryptoUtils::hmacSha256(kDate, region);
        const auto kService = CryptoUtils::hmacSha256(kRegion, service);
        return CryptoUtils::hmacSha256(kService, std::string(kScopeTerminator));
    }

    void SigV4::Sign(http::request<http::string_body> &req, const std::string &accessKeyId, const std::string &secretAccessKey,
                      const std::string &region, const std::string &service) {

        const auto [amzDate, dateStamp] = nowAmzDate();
        req.set("x-amz-date", amzDate);
        req.set("x-amz-content-sha256", CryptoUtils::sha256Hex(req.body()));

        std::string target(req.target());
        const auto q = target.find('?');
        const std::string canonicalUri = q == std::string::npos ? target : target.substr(0, q);
        const std::string canonicalQuery = q == std::string::npos ? "" : CanonicalizeQueryString(target.substr(q + 1));

        const auto canonicalRequest = BuildCanonicalRequest(std::string(req.method_string()), canonicalUri, canonicalQuery,
                                                              headerMap(req), SignedHeaderNames(), req["x-amz-content-sha256"]);

        const std::string credentialScope = dateStamp + "/" + region + "/" + service + "/" + std::string(kScopeTerminator);
        const auto stringToSign = BuildStringToSign(amzDate, credentialScope, CryptoUtils::sha256Hex(canonicalRequest));

        const auto signingKey = DeriveSigningKey(secretAccessKey, dateStamp, region, service);
        const auto signature = toHexLower(CryptoUtils::hmacSha256(signingKey, stringToSign));

        std::string signedHeadersStr;
        for (std::size_t i = 0; i < SignedHeaderNames().size(); ++i) {
            if (i > 0) signedHeadersStr += ";";
            signedHeadersStr += SignedHeaderNames()[i];
        }

        req.set(http::field::authorization, std::string(kAlgorithm) + " Credential=" + accessKeyId + "/" + credentialScope +
                                                     ", SignedHeaders=" + signedHeadersStr + ", Signature=" + signature);
    }

    std::optional<SigV4::VerifyResult> SigV4::Verify(const http::request<http::string_body> &req,
                                                        const std::function<std::optional<std::string>(const std::string &)> &lookupSecret,
                                                        const std::chrono::seconds maxSkew) {

        const auto authHeader = std::string(req[http::field::authorization]);
        const auto parsed = ParseAuthorizationHeader(authHeader);
        if (!parsed.has_value()) return std::nullopt;

        // Fixed policy, not client-negotiated: reject anything that doesn't cover exactly the
        // headers Sign() always signs - see the class comment for why this can't be a
        // client-chosen list here the way real Euclid allows.
        std::string expectedSignedHeaders;
        for (std::size_t i = 0; i < SignedHeaderNames().size(); ++i) {
            if (i > 0) expectedSignedHeaders += ';';
            expectedSignedHeaders += SignedHeaderNames()[i];
        }
        if (parsed->signedHeaders != expectedSignedHeaders) return std::nullopt;

        const auto secret = lookupSecret(parsed->scope.accessKeyId);
        if (!secret.has_value()) return std::nullopt;

        const auto headers = headerMap(req);
        const auto amzDateIt = headers.find("x-amz-date");
        if (amzDateIt == headers.end()) return std::nullopt;
        const auto &amzDate = amzDateIt->second;

        if (amzDate.substr(0, 8) != parsed->scope.dateStamp) return std::nullopt;

        const auto requestTime = parseAmzDate(amzDate);
        if (!requestTime.has_value()) return std::nullopt;
        const auto skew = std::chrono::system_clock::now() - *requestTime;
        if (skew > maxSkew || skew < -maxSkew) return std::nullopt;

        const auto payloadHashIt = headers.find("x-amz-content-sha256");
        if (payloadHashIt == headers.end()) return std::nullopt;
        if (payloadHashIt->second != CryptoUtils::sha256Hex(req.body())) return std::nullopt;

        std::string target(req.target());
        const auto q = target.find('?');
        const std::string canonicalUri = q == std::string::npos ? target : target.substr(0, q);
        const std::string canonicalQuery = q == std::string::npos ? "" : CanonicalizeQueryString(target.substr(q + 1));

        const auto canonicalRequest = BuildCanonicalRequest(std::string(req.method_string()), canonicalUri, canonicalQuery,
                                                              headers, SignedHeaderNames(), payloadHashIt->second);

        const std::string credentialScope = parsed->scope.dateStamp + "/" + parsed->scope.region + "/" + parsed->scope.service + "/" + std::string(kScopeTerminator);
        const auto stringToSign = BuildStringToSign(amzDate, credentialScope, CryptoUtils::sha256Hex(canonicalRequest));

        const auto signingKey = DeriveSigningKey(*secret, parsed->scope.dateStamp, parsed->scope.region, parsed->scope.service);
        const auto expectedSignature = toHexLower(CryptoUtils::hmacSha256(signingKey, stringToSign));

        if (!constantTimeEquals(expectedSignature, parsed->signature)) return std::nullopt;

        return VerifyResult{.accessKeyId = parsed->scope.accessKeyId};
    }

}// namespace Euclid::Core
