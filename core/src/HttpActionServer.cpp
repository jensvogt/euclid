// C++ includes
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>

// Euclid includes
#include <euclid/core/HttpActionServer.h>

#include <euclid/core/Configuration.h>
#include <euclid/core/HttpSignature.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/SigV4.h>
#include <euclid/core/SystemUtils.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/monitoring/MetricEventBus.h>
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

        HttpActionServer::ScopeLookup &scopeLookup() {
            static HttpActionServer::ScopeLookup lookup;
            return lookup;
        }

        HttpActionServer::GrantLookup &grantLookup() {
            static HttpActionServer::GrantLookup lookup;
            return lookup;
        }

        // Empty return means "in scope"; otherwise the message to send back as the 403 body.
        // subject is the already-verified caller (from the JWT/SigV4 check just above this call),
        // used for the per-user grant check once GrantLookup is wired.
        std::string CheckScope(const http::request<http::string_body> &req, const std::optional<std::string> &subject) {

            const auto region = std::string(req["x-euclid-region"]);
            if (const auto configuredRegion = Configuration::instance().getOr<std::string>("euclid.region", ""); !configuredRegion.empty() && configuredRegion != region) {
                return "Region is not permitted";
            }

            const auto accountId = std::string(req["x-euclid-account-id"]);
            const auto ns = std::string(req["x-euclid-namespace"]);

            if (!accountId.empty()) {
                if (scopeLookup()) {
                    // DB is the source of truth once account/namespace management is wired in.
                    if (!scopeLookup()(accountId, ns)) {
                        return ns.empty() ? "Account is not permitted" : "Namespace is not permitted";
                    }
                } else {
                    // Static-config fallback - the only path for modules with no database access
                    // (e.g. ftp). Empty configured list = no restriction (back-compat on upgrade).
                    if (const auto allowedAccountIds = ConfiguredList("euclid.account-ids"); !allowedAccountIds.empty() && std::ranges::find(allowedAccountIds, accountId) == allowedAccountIds.end()) {
                        return "Account is not permitted";
                    }
                    if (const auto allowedNamespaces = ConfiguredList("euclid.namespaces"); !ns.empty() && !allowedNamespaces.empty() && std::ranges::find(allowedNamespaces, ns) == allowedNamespaces.end()) {
                        return "Namespace is not permitted";
                    }
                }
            }

            // Per-user grant check - only enforced once GrantLookup is wired and the request
            // actually names an account; account-agnostic actions (e.g. login, get-metrics)
            // typically send no x-euclid-account-id and are exempt.
            if (grantLookup() && !accountId.empty() && subject.has_value()) {
                if (!grantLookup()(*subject, accountId, ns)) {
                    return ns.empty() ? "Not authorized for this account" : "Not authorized for this account/namespace";
                }
            }

            return {};
        }

        HttpActionServer::AccessKeyLookup &accessKeyLookup() {
            static HttpActionServer::AccessKeyLookup lookup;
            return lookup;
        }

        constexpr auto kCpuUsagePeriod = std::chrono::seconds(60);

        // Previous SystemUtils::ReadCpuTimes() reading, so recordCpuUsage() can compute a
        // delta-based percentage rather than a since-boot average. There's only ever one
        // HttpActionServer-derived instance per process, so file-local state is simplest - same
        // reasoning as EmoServer's own accumulators.
        std::mutex &cpuTimesMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::optional<SystemUtils::CpuTimes> &previousCpuTimes() {
            static std::optional<SystemUtils::CpuTimes> previous;
            return previous;
        }

        // Records this process's system CPU usage since the previous call as a "euclid-cpu-usage"
        // gauge, labelled with this module's serviceName - picked up by MonitoringCollector and
        // pushed to the monitoring module by MetricsPusher, same as any other MetricEventBus
        // metric. The first call after process start only primes the previous-reading state,
        // since a delta needs two samples.
        void recordCpuUsage(const std::string &serviceName) {

            const auto current = SystemUtils::ReadCpuTimes();
            if (!current.has_value()) return;

            std::lock_guard lock(cpuTimesMutex());
            const auto previous = previousCpuTimes();
            previousCpuTimes() = current;
            if (!previous.has_value()) return;

            const auto totalDelta = current->total - previous->total;
            const auto idleDelta = current->idle - previous->idle;
            if (totalDelta == 0) return;

            const auto percent = 100.0 * static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
            Monitoring::MetricEventBus::instance().sigMetricGauge("euclid-cpu-usage", "module", serviceName, percent);
        }

        // Records this process's current memory usage as three gauges, all labelled with this
        // module's serviceName - real/virtual memory in MB, plus real memory as a percentage of
        // the system's total RAM. Unlike recordCpuUsage(), this is a direct point-in-time
        // reading, so there's no state to prime on the first call.
        void recordMemoryUsage(const std::string &serviceName) {

            const auto usage = SystemUtils::ReadMemoryUsage();
            if (!usage.has_value()) return;

            auto &bus = Monitoring::MetricEventBus::instance();
            bus.sigMetricGauge("euclid-memory-usage-real-mb", "module", serviceName, usage->realMb);
            bus.sigMetricGauge("euclid-memory-usage-virtual-mb", "module", serviceName, usage->virtualMb);
            bus.sigMetricGauge("euclid-memory-usage-percent", "module", serviceName, usage->percentOfTotal);
        }
    }// namespace

    HttpActionServer::HttpActionServer(const std::string &serviceName, std::string socketPath, const int threads) : UnixSocketServer(serviceName, std::move(socketPath), threads) {
        Monitoring::MonitoringCollector::instance().Start();

#ifdef __linux__
        auto &scheduler = Scheduler::instance();
        scheduler.Start();
        const auto resourceUsagePeriod = std::chrono::seconds(Configuration::instance().getOr<long>("euclid.monitoring.cpu-usage-period", kCpuUsagePeriod.count()));
        _resourceUsageTaskId = scheduler.SchedulePeriodic("resource-usage-" + serviceName, [serviceName] {
            recordCpuUsage(serviceName);
            recordMemoryUsage(serviceName);
        }, std::chrono::duration_cast<milliseconds>(resourceUsagePeriod));
#endif
    }

    HttpActionServer::~HttpActionServer() {
        Scheduler::instance().Cancel(_resourceUsageTaskId);
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
        return Configuration::instance().getOr<std::string>("euclid.modules.eam.jwt-secret", kInsecureDefaultJwtSecret);
    }

    bool HttpActionServer::ValidateJwtSecret() {
        const auto secret = JwtSecret();

        if (secret == kInsecureDefaultJwtSecret) {
            log_error << "euclid.modules.eam.jwt-secret is not configured - refusing to start with the insecure default secret; "
                       << "set a random secret of at least " << kMinJwtSecretLength << " bytes (e.g. `openssl rand -hex 32`)";
            return false;
        }

        if (secret.size() < kMinJwtSecretLength) {
            log_error << "euclid.modules.eam.jwt-secret is only " << secret.size() << " bytes; HS256 requires at least "
                       << kMinJwtSecretLength << " bytes - generate one with `openssl rand -hex 32`";
            return false;
        }

        return true;
    }

    void HttpActionServer::SetAccessKeyLookup(AccessKeyLookup lookup) {
        accessKeyLookup() = std::move(lookup);
    }

    void HttpActionServer::SetScopeLookup(ScopeLookup lookup) {
        scopeLookup() = std::move(lookup);
    }

    void HttpActionServer::SetGrantLookup(GrantLookup lookup) {
        grantLookup() = std::move(lookup);
    }

    HttpActionServer::AuthResult HttpActionServer::Authenticate(const http::request<http::string_body> &req) {

        const auto header = std::string(req[http::field::authorization]);

        std::optional<std::string> subject;

        // Checked before Authorization: an RFC 9421 signature lives in its own headers and a
        // client may well present it with no Authorization header at all, so keying off that one
        // would never reach this branch.
        if (HttpSignature::IsSigned(req)) {

            std::string resolvedUserId;
            const auto lookupSecret = [&](const std::string &accessKeyId) -> std::optional<std::string> {
                const auto record = accessKeyLookup() ? accessKeyLookup()(accessKeyId) : std::nullopt;
                if (!record.has_value()) return std::nullopt;
                resolvedUserId = record->userId;
                return record->secretAccessKey;
            };
            if (!HttpSignature::Verify(req, lookupSecret).has_value()) {
                return {.subject = std::nullopt, .denialReason = "Signature does not match"};
            }
            subject = resolvedUserId;

        } else if (constexpr std::string_view bearerPrefix = "Bearer "; header.starts_with(bearerPrefix)) {

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

        if (auto denialReason = CheckScope(req, subject); !denialReason.empty()) {
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