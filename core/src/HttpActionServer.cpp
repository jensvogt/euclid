// C++ includes
#include <algorithm>

// Euclid includes
#include <euclid/core/HttpActionServer.h>

#include <euclid/core/Configuration.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/SigV4.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/monitoring/MonitoringCollector.h>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::Core {

    namespace beast = boost::beast;
    namespace http = beast::http;

    namespace {
        // Fallback used only when the operator hasn't configured a secret - ValidateJwtSecret()
        // refuses to start the service while this is in effect.
        constexpr auto kInsecureDefaultJwtSecret = "change-me-insecure-default-secret";

        // HS256 = HMAC-SHA256; RFC 2104 / FIPS 198-1 recommend a key at least as long as the
        // hash output, i.e. 32 bytes. Shorter keys make the key itself brute-forceable rather
        // than the signature.
        constexpr std::size_t kMinJwtSecretLength = 32;

        // Deployments that haven't configured euclid.account-id/euclid.region/euclid.namespaces
        // yet aren't scoped by that dimension - an empty/missing list or region means "no
        // restriction", so this check doesn't lock operators out on upgrade.
        std::vector<std::string> ConfiguredList(const std::string &path) {
            try {
                return Configuration::instance().getArray<std::string>(path);
            } catch (...) {
                return {};
            }
        }

        // Empty return means "in scope"; otherwise the message to send back as the 403 body.
        std::string CheckScope(const http::request<http::string_body> &req) {

            const auto region = std::string(req["x-euclid-region"]);
            if (const auto configuredRegion = Configuration::instance().getOr<std::string>("euclid.region", ""); !configuredRegion.empty() && configuredRegion != region) {
                return "Region is not permitted";
            }

            const auto accountId = std::string(req["x-euclid-account-id"]);
            if (const auto allowedAccountIds = ConfiguredList("euclid.account-id"); !allowedAccountIds.empty() && std::ranges::find(allowedAccountIds, accountId) == allowedAccountIds.end()) {
                return "Account is not permitted";
            }

            const auto ns = std::string(req["x-euclid-namespace"]);
            if (const auto allowedNamespaces = ConfiguredList("euclid.namespaces"); !ns.empty() && !allowedNamespaces.empty() && std::ranges::find(allowedNamespaces, ns) == allowedNamespaces.end()) {
                return "Namespace is not permitted";
            }

            return {};
        }

        HttpActionServer::AccessKeyLookup &accessKeyLookup() {
            static HttpActionServer::AccessKeyLookup lookup;
            return lookup;
        }
    }// namespace

    HttpActionServer::HttpActionServer(std::string serviceName, std::string socketPath, const int threads)
        : UnixSocketServer(std::move(serviceName), std::move(socketPath), threads) {
        Monitoring::MonitoringCollector::instance().Start();
    }

    http::response<http::string_body> HttpActionServer::JsonResponse(const http::request<http::string_body> &req, const http::status status, std::string body) {
        http::response<http::string_body> res{status, req.version()};
        res.set(http::field::content_type, "application/json");
        // Generated fresh per response (not echoed from any client-supplied value) so it always
        // reflects the request this server actually processed, mirroring AWS's x-amzn-RequestId.
        res.set("x-euclid-request-id", RequestId());
        res.keep_alive(req.keep_alive());
        res.body() = std::move(body);
        res.prepare_payload();
        return res;
    }

    http::response<http::string_body> HttpActionServer::ErrorResponse(const http::request<http::string_body> &req, const http::status status, const std::string_view message) {
        const boost::json::object body{{"error", std::string(message)}};
        return JsonResponse(req, status, boost::json::serialize(body));
    }

    std::string HttpActionServer::JwtSecret() {
        return Configuration::instance().getOr<std::string>("euclid.modules.access.jwt-secret", kInsecureDefaultJwtSecret);
    }

    bool HttpActionServer::ValidateJwtSecret() {
        const auto secret = JwtSecret();

        if (secret == kInsecureDefaultJwtSecret) {
            log_error << "euclid.modules.access.jwt-secret is not configured - refusing to start with the insecure default secret; "
                       << "set a random secret of at least " << kMinJwtSecretLength << " bytes (e.g. `openssl rand -hex 32`)";
            return false;
        }

        if (secret.size() < kMinJwtSecretLength) {
            log_error << "euclid.modules.access.jwt-secret is only " << secret.size() << " bytes; HS256 requires at least "
                       << kMinJwtSecretLength << " bytes - generate one with `openssl rand -hex 32`";
            return false;
        }

        return true;
    }

    void HttpActionServer::SetAccessKeyLookup(AccessKeyLookup lookup) {
        accessKeyLookup() = std::move(lookup);
    }

    HttpActionServer::AuthResult HttpActionServer::Authenticate(const http::request<http::string_body> &req) {

        const auto header = std::string(req[http::field::authorization]);

        std::optional<std::string> subject;

        if (constexpr std::string_view bearerPrefix = "Bearer "; header.starts_with(bearerPrefix)) {

            const auto token = header.substr(bearerPrefix.size());
            const auto secret = JwtSecret();
            subject = JwtUtils::VerifyToken(token, secret);
            if (!subject.has_value()) {
                return {.subject = std::nullopt, .tokenExpired = JwtUtils::IsTokenExpired(token, secret)};
            }

        } else if (constexpr std::string_view sigV4Prefix = "AWS4-HMAC-SHA256 "; header.starts_with(sigV4Prefix)) {

            std::string resolvedUserId;
            const auto lookupSecret = [&](const std::string &accessKeyId) -> std::optional<std::string> {
                const auto record = accessKeyLookup() ? accessKeyLookup()(accessKeyId) : std::nullopt;
                if (!record.has_value()) return std::nullopt;
                resolvedUserId = record->userId;
                return record->secretAccessKey;
            };
            if (!SigV4::Verify(req, lookupSecret).has_value()) {
                return {.subject = std::nullopt, .denialReason = "Signature does not match"};
            }
            subject = resolvedUserId;

        } else {
            return {};
        }

        if (auto denialReason = CheckScope(req); !denialReason.empty()) {
            return {.subject = std::nullopt, .denialReason = std::move(denialReason)};
        }

        return {.subject = std::move(subject)};
    }

    http::response<http::string_body> HttpActionServer::Unauthorized(const http::request<http::string_body> &req, const AuthResult &auth) {
        if (!auth.denialReason.empty()) {
            return ErrorResponse(req, http::status::forbidden, auth.denialReason);
        }
        return ErrorResponse(req, http::status::unauthorized, auth.tokenExpired ? "Bearer token expired" : "Missing or invalid bearer token");
    }

    std::optional<http::response<http::string_body> > HttpActionServer::ParseJsonBody(const http::request<http::string_body> &req, boost::json::value &out) {
        boost::system::error_code ec;
        out = boost::json::parse(req.body(), ec);
        if (ec) {
            return ErrorResponse(req, http::status::bad_request, "Invalid JSON body");
        }
        return std::nullopt;
    }

    std::string HttpActionServer::RequestId() {
        return UuidUtils::CreateRandomUuid();
    }

    http::response<http::string_body> HttpActionServer::MetricsResponse(const http::request<http::string_body> &req) {
        boost::json::array items;
        for (const auto &sample: Monitoring::MonitoringCollector::instance().Collect()) {
            items.push_back(boost::json::object{
                    {"name", sample.name},
                    {"labelName", sample.labelName},
                    {"labelValue", sample.labelValue},
                    {"value", sample.value},
                    {"type", sample.isRate ? "rate" : "gauge"}
            });
        }
        return JsonResponse(req, http::status::ok, boost::json::serialize(boost::json::object{{"items", items}}));
    }

}// namespace Euclid::Core