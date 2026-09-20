// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/Permissions.h>

// C++ includes
#include <algorithm>
#include <ranges>

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


        HttpActionServer::WorkerThreadsLookup &workerThreadsLookup() {
            static HttpActionServer::WorkerThreadsLookup lookup;
            return lookup;
        }

        HttpActionServer::RequestRewriter &requestRewriter() {
            static HttpActionServer::RequestRewriter rewriter;
            return rewriter;
        }


        // Empty return means "in scope"; otherwise the message to send back as the 403 body.
        // subject is the already-verified caller (from the JWT/SigV4 check just above this call),
        // used for the per-user grant check once GrantLookup is wired.
        std::string CheckScope(const http::request<http::string_body> &req, [[maybe_unused]] const std::optional<std::string> &subject) {

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

            // What used to be a per-user grant check lived here, reading User::accountGrants. That
            // field is gone: whether a caller may work in an account and namespace is now a
            // property of the role bindings they hold, and Authorize() decides it for every request
            // rather than this deciding it for the ones that happen to name an account. What is
            // left here is deployment scope - whether this euclid serves that account at all.

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

    int HttpActionServer::ConfiguredWorkerThreads(const std::string &module, const int fallback) {

        // What was asked for at runtime, if anything, in preference to the configuration file: a
        // count set through "emm set-threads" is the more recent statement of intent, and is the
        // only one an operator can change without editing a file on every host.
        long configured = -1;
        if (workerThreadsLookup()) {
            try {
                configured = workerThreadsLookup()(module);
            } catch (const std::exception &e) {
                // Never fatal. A module that cannot reach the database has a configuration file to
                // fall back on, and refusing to start over a tuning parameter would be the worse
                // of the two outcomes.
                log_warning << "Could not read the configured worker threads for " << module << ", using the configuration file, error: " << e.what();
            }
        }
        if (configured < 0) configured = Configuration::instance().getOr<long>("euclid.modules." + module + ".threads", fallback);

        // Clamped rather than trusted, by the same rule every server on this side uses - a module
        // that fails to start is a worse answer to a misconfigured thread count than one that runs
        // with a sane one and says so.
        const auto threads = UnixSocketServer::ClampWorkerThreads(configured);
        if (threads != configured) {
            log_warning << "Module " << module << " asked for " << configured << " worker threads, using " << threads;
        }
        return threads;
    }


    // ── The role gate ────────────────────────────────────────────────────────

    namespace {

        // Null until a process wires one. A module that never does is never refused anything,
        // which is what keeps the gate additive until an installation turns it on.
        HttpActionServer::AuthorizationLookup g_authorizationLookup;
        HttpActionServer::AuditSink g_auditSink;
        HttpActionServer::ResourceAuthorizationLookup g_resourceAuthorizationLookup;

    }// namespace

    void HttpActionServer::SetAuthorizationLookup(AuthorizationLookup lookup) {
        g_authorizationLookup = std::move(lookup);
    }


    void HttpActionServer::SetResourceAuthorizationLookup(ResourceAuthorizationLookup lookup) {
        g_resourceAuthorizationLookup = std::move(lookup);
    }

    bool HttpActionServer::IsResourceAuthorized(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                                const std::string &resourceErn) {

        if (!g_resourceAuthorizationLookup) return true;

        return g_resourceAuthorizationLookup(req, std::string(req["x-euclid-target"]),
                                             std::string(req["x-euclid-action"]), resourceErn)
                .allowed;
    }

    std::optional<boost::beast::http::response<boost::beast::http::string_body> >
    HttpActionServer::AuthorizeResource(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                        const std::string &resourceErn) {

        // A process that registered no lookup is not refused anything, the same way the gate above
        // treats one - a tool built on this class is not a module and has no grants to consult.
        if (!g_resourceAuthorizationLookup) return std::nullopt;

        const auto target = std::string(req["x-euclid-target"]);
        const auto action = std::string(req["x-euclid-action"]);

        const auto decision = g_resourceAuthorizationLookup(req, target, action, resourceErn);
        if (decision.allowed) return std::nullopt;

        log_info << "authorization: refused " << target << ":" << action << " on " << resourceErn
                 << ", user: " << std::string(req["x-euclid-user-id"]) << ", reason: " << decision.reason;

        return ErrorResponse(req, boost::beast::http::status::forbidden, decision.reason);
    }


    void HttpActionServer::RequireEnforcingAuthorization() {

        const auto configured = Configuration::instance().getOr<std::string>("euclid.authorization.mode", "enforce");
        if (configured == "enforce") return;

        // "legacy" and "shadow" both meant "let the per-user grant lists decide". Those lists are
        // gone, so honouring either would authorize nobody - every authenticated caller allowed
        // everything, with nothing in any log to say so. Refusing to start is the loudest, earliest
        // and least damaging way to say that the configuration is from before the roles release.
        throw std::runtime_error("euclid.authorization.mode is '" + configured +
                                 "', which no longer exists: the per-user accountGrants/resourceGrants it referred to "
                                 "have been replaced by roles. Set it to 'enforce', or remove it. See "
                                 "docs/role-concept.md.");
    }

    std::optional<boost::beast::http::response<boost::beast::http::string_body> >
    HttpActionServer::Authorize(const boost::beast::http::request<boost::beast::http::string_body> &req) {

        // A process that registered no lookup is never refused anything. Every module registers one
        // at startup; a tool built on this class is not a module and has no grants to consult.
        if (!g_authorizationLookup) return std::nullopt;

        const auto target = std::string(req["x-euclid-target"]);
        const auto action = std::string(req["x-euclid-action"]);

        // emm and emd are not gated by roles because no role can name their actions - they gate
        // themselves, by the administrator group and by being internal respectively. Sending them
        // through here would refuse every one of their requests, since Permissions holds nothing
        // for either.
        if (std::ranges::contains(Permissions::UnbindableModules(), target)) return std::nullopt;

        const auto decision = g_authorizationLookup(req, target, action);
        if (decision.allowed) return std::nullopt;

        log_info << "authorization: refused " << target << ":" << action
                 << ", user: " << std::string(req["x-euclid-user-id"]) << ", reason: " << decision.reason;

        return ErrorResponse(req, boost::beast::http::status::forbidden, decision.reason);
    }

    void HttpActionServer::SetAuditSink(AuditSink sink) {
        g_auditSink = std::move(sink);
    }

    namespace {

        // Modules that are machinery rather than anything anybody runs.
        //
        // EMO is the monitoring store. Every instance of every module pushes to it on a timer, and
        // none of its actions read as reads - push-metrics writes, and "list" and "average" miss
        // the list-/get- rule for want of a hyphen - so all of it would be kept. Measured on the
        // development installation before this existed: 1,610 emo:list and 125 emo:push-metrics
        // against a few hundred real commands, which is a trail made mostly of the machinery.
        //
        // EMD is the document store underneath every other module's every read and write,
        // including the audit's own. Its main() does not wire the sink at all, so this is belt and
        // braces - but the sink went into thirteen modules by copying one line, and that is exactly
        // how emd would acquire it by accident.
        //
        // Excluded whatever the status, unlike everything else here. A refusal is normally the
        // entry an audit exists for, but a metrics push that starts failing fails on a timer too:
        // recording those would turn one broken pusher into a flood, and the module log is where
        // that belongs.
        constexpr std::array kMachineryModules{
                std::string_view{"emo"},
                std::string_view{"emd"},
        };

        // Actions that carry data rather than decide anything, excluded by name.
        //
        // The machinery list above excludes whole modules; these are individual actions inside
        // modules that are otherwise very much worth auditing. What they have in common is that
        // they repeat per unit of data rather than per thing a person did, so one intelligible
        // operation becomes thousands of entries and buries the operations either side of it.
        //
        // Measured on a development installation over twenty minutes: 205,631 upload-part,
        // 101,736 receive-messages and 95,248 delete-message, against a few thousand entries for
        // everything an operator would actually search for. The writer could not keep up and began
        // discarding the oldest entries - 46,000 of them in one process - so the volume was not
        // merely noisy, it was destroying the trail it was part of.
        //
        // Each one is already bracketed by something that IS recorded, which is what makes them
        // safe to leave out rather than merely expensive to keep:
        //
        //   upload-part      - create-upload and complete-upload record the upload, with its key,
        //                      its caller and its outcome. A 12 GB file is 1,479 parts; the parts
        //                      are how the bytes arrived, not what was done.
        //   receive-messages - a consumer polling its own queue, on a timer, forever. It is the
        //                      absence of a poll that would be worth knowing about.
        //   delete-message   - the other half of the same poll: a consumer acknowledging work it
        //                      was given. send-message, which is somebody putting work in, stays.
        //
        // A failure still goes in the trail. These are dropped only when they succeeded, so a
        // refused upload-part or a delete-message that 403s is recorded exactly as before - that
        // is the case an audit exists for, and it does not come on a timer.
        constexpr std::array kDataPlaneActions{
                std::string_view{"upload-part"},
                std::string_view{"receive-messages"},
                std::string_view{"delete-message"},
        };

    }// namespace

    bool HttpActionServer::ShouldAudit(const std::string_view target, const std::string_view action, const long status) {

        if (std::ranges::contains(kMachineryModules, target)) return false;

        const bool succeeded = status >= 200 && status < 300;

        // Only when it worked - see kDataPlaneActions. A part that was refused is exactly the kind
        // of thing the trail is for, and unlike a successful one it does not arrive by the
        // thousand.
        if (succeeded && std::ranges::contains(kDataPlaneActions, action)) return false;

        if (!succeeded) return true;
        if (!Permissions::IsRead(action)) return true;

        return Configuration::instance().getOr<bool>("euclid.modules.ead.audit-reads", false);
    }

    void HttpActionServer::RecordAudit(const http::request<http::string_body> &req, const long status) {

        if (!g_auditSink) return;

        const auto target = std::string(req["x-euclid-target"]);
        const auto action = std::string(req["x-euclid-action"]);
        if (!ShouldAudit(target, action, status)) return;

        try {
            // Identity from the headers the gateway verified, never from the body: a record of who
            // did something that the doer can write is not a record of anything.
            g_auditSink(AuditRecord{
                    .accountId = std::string(req["x-euclid-account-id"]),
                    .nameSpace = std::string(req["x-euclid-namespace"]),
                    .userId = std::string(req["x-euclid-user-id"]),
                    .moduleName = target,
                    .command = action,
                    .parameters = req.body(),
                    .status = status});

        } catch (const std::exception &e) {
            // Swallowed, and this is the point of the try: recording a command must never be a way
            // to fail it. A trail that is missing an entry is a smaller problem than a request
            // that failed because the trail could not be written.
            log_warning << "Could not record audit event, action: " << action << ", error: " << e.what();
        }
    }

    boost::beast::http::response<boost::beast::http::string_body>
    HttpActionServer::Dispatch(const boost::beast::http::request<boost::beast::http::string_body> &req) {

        if (auto refusal = Authorize(req)) {
            // Recorded before it is returned, and recorded whatever the action was: a refusal is
            // the entry an audit is kept for.
            RecordAudit(req, refusal->result_int());
            return std::move(*refusal);
        }

        auto response = DispatchAction(req);
        RecordAudit(req, response.result_int());
        return response;
    }

    HttpActionServer::HttpActionServer(const std::string &serviceName, std::string socketPath, const int threads) : UnixSocketServer(serviceName, std::move(socketPath), threads) {
        RequireEnforcingAuthorization();

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

    std::optional<HttpActionServer::AccessKeyRecord> HttpActionServer::LookupAccessKey(const std::string &accessKeyId) {
        return accessKeyLookup() ? accessKeyLookup()(accessKeyId) : std::nullopt;
    }

    void HttpActionServer::SetAccessKeyLookup(AccessKeyLookup lookup) {
        accessKeyLookup() = std::move(lookup);
    }

    void HttpActionServer::SetScopeLookup(ScopeLookup lookup) {
        scopeLookup() = std::move(lookup);
    }


    void HttpActionServer::SetWorkerThreadsLookup(WorkerThreadsLookup lookup) {
        workerThreadsLookup() = std::move(lookup);
    }

    void HttpActionServer::SetRequestRewriter(RequestRewriter rewriter) {
        requestRewriter() = std::move(rewriter);
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
        // After the parse, so a rewriter only ever sees a well-formed body, and before any handler
        // reads it - which is the whole point: what a handler gets is already normalised.
        if (requestRewriter()) requestRewriter()(req, out);
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