#pragma once

// C++ includes
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json/fwd.hpp>

// Euclid includes
#include <euclid/core/UnixSocketServer.h>

namespace Euclid::Core {

    /**
     * @brief Base class for module HTTP servers that dispatch "action" style requests
     * (e.g. access, SQS), on top of a Unix domain socket.
     *
     * Adds the response-building and bearer-token authentication helpers shared by those
     * servers - Dispatch() is still left to subclasses to implement.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class HttpActionServer : public UnixSocketServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * Also starts Core::Monitoring::MonitoringCollector, so every module built on this base
         * class collects Core::Monitoring::MonitoringTimer/MetricEventBus metrics without each
         * one having to remember to start it itself. Also schedules a periodic task (Linux only)
         * that records this process's system CPU usage ("euclid-cpu-usage") and memory usage
         * ("euclid-memory-usage-real-mb"/"-virtual-mb"/"-percent") as gauges, all labelled with
         * this module's serviceName, so they're pushed to the monitoring module the same way as
         * every other MetricEventBus-recorded metric - see MetricsPusher.
         *
         * @param serviceName name used in log lines and the accept loop's thread name.
         * @param socketPath  Unix domain socket path to listen on.
         * @param threads     number of io_context worker threads.
         */
        HttpActionServer(const std::string &serviceName, std::string socketPath, int threads);

        /**
         * @brief How many worker threads this module should run, from its configuration.
         *
         * @par
         * Reads "euclid.modules.&lt;module&gt;.threads", alongside the minInstances/maxInstances
         * that size the pool of processes - this sizes the pool of threads inside one of them.
         * They answer different questions: instances are how much of a module the manager runs,
         * threads are how many requests one instance can be in the middle of.
         *
         * @par
         * It matters most for the modules whose busiest action deliberately waits - EES's
         * receive-events and EQS's receive-messages hold a thread for as long as the caller asked
         * them to wait - where too few threads means requests that are not waiting queue behind
         * the ones that are. Core::LongPollSlots keeps one thread out of the waiting, so the
         * configured figure is how many callers may wait at once, plus one.
         *
         * @par
         * A count set through "euclid-cli emm set-threads" wins over the configuration file, so
         * the figure can be changed without editing euclid.json on every host and without the
         * change being lost at the next package upgrade. Nothing is read from the database until
         * SetWorkerThreadsLookup() has been wired, so a process that never wires it - or one whose
         * repository is not up yet - behaves exactly as it did before.
         *
         * @param module the module's key in the configuration, e.g. "ees"
         * @param fallback what to use when the key is absent, which is the ordinary case
         * @return the configured count, clamped to something a machine can actually run
         */
        [[nodiscard]]
        static int ConfiguredWorkerThreads(const std::string &module, int fallback);

        /**
         * @brief Callback ConfiguredWorkerThreads() uses to find a thread count set at runtime.
         *
         * @param module the module's name, e.g. "ees"
         * @return the count somebody asked for, or -1 if nobody has.
         */
        using WorkerThreadsLookup = std::function<int(const std::string &module)>;

        /**
         * @brief Registers the runtime thread-count lookup ConfiguredWorkerThreads() consults.
         *
         * core doesn't depend on database (database depends on core), so each module process wires
         * in its own repository-backed lookup at startup - see Database::WireWorkerThreadsLookup()
         * - after RepositoryFactory::initialize() and before it constructs its server. Until this
         * is called only the configuration file is consulted.
         *
         * @param lookup resolves the thread count set for a module, or -1 for none.
         */
        static void SetWorkerThreadsLookup(WorkerThreadsLookup lookup);

        /**
         * @brief Callback ParseJsonBody() applies to every parsed request body.
         *
         * @param req the request the body came from, for its headers.
         * @param body the parsed body, rewritten in place.
         */
        using RequestRewriter = std::function<void(const boost::beast::http::request<boost::beast::http::string_body> &req, boost::json::value &body)>;

        /**
         * @brief Registers a rewriter applied to every request body this process parses.
         *
         * @par
         * Exists so a module can normalise what clients send in one place instead of in every
         * handler - specifically, so a resource named by name can be resolved to its full ERN
         * (see Core::resolveErn) before any handler sees it. Doing that per handler would mean
         * remembering it in forty places, and the ones nobody remembered would be exactly the
         * ones that behaved differently.
         *
         * @par
         * Applied only to bodies that parsed cleanly, so a rewriter never sees a malformed
         * request. Until this is called nothing is rewritten, which is what every module that
         * does not wire one keeps doing.
         *
         * @param rewriter invoked with each parsed body; may modify it in place.
         */
        static void SetRequestRewriter(RequestRewriter rewriter);

        /**
         * @brief Cancels the periodic CPU/memory usage collection task.
         */
        ~HttpActionServer() override;

        /**
         * @brief Builds an HTTP response with a JSON body.
         *
         * Always calls prepare_payload(), even for an empty body: this sets
         * Content-Length: 0, which a keep-alive HTTP/1.1 response needs to signal "no
         * body" - without it the client has no way to tell the response is complete and
         * will hang waiting for more data.
         *
         * @param req    original request, used for the HTTP version and keep-alive flag
         * @param status HTTP status code
         * @param body   serialized JSON body, defaults to empty
         * @return response with content-type: application/json
         */
        [[nodiscard]]
        static boost::beast::http::response<boost::beast::http::string_body>
        JsonResponse(const boost::beast::http::request<boost::beast::http::string_body> &req, boost::beast::http::status status, std::string body = {});

        /**
         * @brief Builds a JSON error response: {"error": message}.
         *
         * @param req     original request, used for the HTTP version and keep-alive flag
         * @param status  HTTP status code
         * @param message error message, placed under the "error" key
         * @return response with content-type: application/json
         */
        [[nodiscard]]
        static boost::beast::http::response<boost::beast::http::string_body>
        ErrorResponse(const boost::beast::http::request<boost::beast::http::string_body> &req, boost::beast::http::status status, std::string_view message);

        /**
         * @brief Result of Authenticate(): the verified subject (the token's "sub" claim,
         * e.g. a user ID) if the request carried a valid bearer token and its
         * x-euclid-account-id/x-euclid-region/x-euclid-namespace headers are in scope for this
         * deployment, or std::nullopt otherwise - tokenExpired set if that was specifically why
         * token verification failed, denialReason set (instead) if the token verified but the
         * request was out of scope.
         *
         * Only the token itself is checked here - callers that need the actual user entity
         * (e.g. to check admin privileges) still have to look it up by subject themselves,
         * since this base class doesn't know about any module's user/entity model.
         */
        struct AuthResult {
            std::optional<std::string> subject;
            bool tokenExpired{false};
            std::string denialReason;
        };

        /**
         * @brief Resolves and verifies the request's Authorization header - either
         * "Bearer <jwt>" or "AWS4-HMAC-SHA256 ..." (SigV4, see Core::SigV4) - then checks the
         * request's x-euclid-account-id, x-euclid-region and x-euclid-namespace headers against
         * this deployment's configured euclid.account-id, euclid.region and euclid.namespaces.
         *
         * SigV4 verification requires resolving an access key ID to its owner; see
         * SetAccessKeyLookup() for how that's wired in, since this class (core) can't depend on
         * the database module that actually stores access keys.
         *
         * @param req request to authenticate
         * @return the verified subject, or an empty AuthResult (tokenExpired set if verification
         * failed because a bearer token was missing/invalid/expired, denialReason set if the
         * credential verified but the request's account/region/namespace was out of scope).
         */
        [[nodiscard]]
        static AuthResult Authenticate(const boost::beast::http::request<boost::beast::http::string_body> &req);

        /**
         * @brief What Authenticate() needs about the owner of a SigV4 access key ID.
         */
        struct AccessKeyRecord {
            std::string secretAccessKey;
            std::string userId;
        };

        /**
         * @brief Callback Authenticate() uses to resolve a SigV4 access key ID to its owner.
         */
        using AccessKeyLookup = std::function<std::optional<AccessKeyRecord>(const std::string &accessKeyId)>;

        /**
         * @brief Registers the access-key lookup Authenticate() uses to verify SigV4 signatures.
         *
         * core doesn't depend on database (database depends on core), so each process wires in
         * its own Database::RepositoryFactory-backed lookup at startup, once its repository is
         * initialized. Until this is called, SigV4-signed requests are rejected as unauthorized.
         *
         * @param lookup resolves an access key ID to its secret/owner, or std::nullopt if unknown.
         */
        static void SetAccessKeyLookup(AccessKeyLookup lookup);

        /**
         * @brief Callback CheckScope (inside Authenticate()) uses to verify an account/namespace
         * exists, once account/namespace management has a database behind it.
         *
         * @param accountId account to check
         * @param ns namespace to check within accountId, or empty to check the account only
         * @return true if in scope (exists), false to deny the request
         */
        using ScopeLookup = std::function<bool(const std::string &accountId, const std::string &ns)>;

        /**
         * @brief Registers the scope lookup Authenticate() uses to validate
         * x-euclid-account-id/x-euclid-namespace against the database instead of the static
         * euclid.account-ids/euclid.namespaces config lists.
         *
         * core doesn't depend on database (database depends on core), so - same pattern as
         * SetAccessKeyLookup() - each process wires in its own Database::RepositoryFactory-backed
         * lookup at startup. Until this is called, scope is checked against static config only
         * (see ConfiguredList in HttpActionServer.cpp), which is what keeps modules with no
         * database access (e.g. ftp) working unchanged.
         *
         * @param lookup resolves whether accountId/ns exist.
         */
        static void SetScopeLookup(ScopeLookup lookup);

        /**
         * @brief Callback CheckScope (inside Authenticate()) uses to verify the authenticated
         * subject is actually granted access to the requested account/namespace.
         *
         * @param userId authenticated subject (already resolved from a JWT or SigV4 signature)
         * @param accountId account being accessed
         * @param ns namespace being accessed, or empty for an account-level-only request
         * @return true if authorized (e.g. a global/account admin, or a matching grant), false to deny
         */
        using GrantLookup = std::function<bool(const std::string &userId, const std::string &accountId, const std::string &ns)>;

        /**
         * @brief Registers the grant lookup Authenticate() uses to enforce per-user
         * account/namespace grants.
         *
         * Only enforced once wired, and only for requests that carry a non-empty
         * x-euclid-account-id - deployments/modules that never call this (e.g. ftp) are
         * unaffected, same backward-compatibility contract as SetScopeLookup().
         *
         * @param lookup resolves whether userId is authorized for accountId/ns.
         */
        static void SetGrantLookup(GrantLookup lookup);

        /**
         * @brief Callback IsResourceAllowed() uses to decide whether a caller may act on one
         * particular resource.
         *
         * @param userId authenticated subject.
         * @param resourceErn ERN of the resource the request names, e.g. a bucket or a queue.
         * @return true if the caller may act on it.
         */
        using ResourceLookup = std::function<bool(const std::string &userId, const std::string &resourceErn)>;

        /**
         * @brief Registers the resource lookup IsResourceAllowed() consults.
         *
         * core doesn't depend on database (database depends on core), so each process wires in
         * its own Database::RepositoryFactory-backed lookup at startup, once its repository is
         * initialized. Until this is called, no request is refused on resource grounds - which is
         * what keeps a module that has not been taught about this behaving as it always did.
         *
         * @param lookup resolves whether a user may act on a resource.
         */
        static void SetResourceLookup(ResourceLookup lookup);

        /**
         * @brief Whether an authenticated caller may act on a resource.
         *
         * @par
         * Called by a handler once it has resolved *which* resource the request is about, which
         * is why this cannot live in Authenticate(): the resource is named in a request body or
         * a header that only the handler understands. Account and namespace scope are settled
         * before a handler runs; this is the narrower question of whether this particular bucket
         * or queue is one the caller was given.
         *
         * @param userId authenticated subject.
         * @param resourceErn ERN of the resource, e.g. "ern:esm:...:bucket:inbox".
         * @return true if the caller may act on it, and whenever no lookup is wired.
         */
        [[nodiscard]]
        static bool IsResourceAllowed(const std::string &userId, const std::string &resourceErn);

        /**
         * @brief Builds the error response for a failed Authenticate() call: 403 with
         * denialReason if the token verified but the request was out of scope, otherwise 401
         * worded according to whether the token was expired or simply missing/invalid.
         *
         * @param req  original request, used for the HTTP version and keep-alive flag
         * @param auth result of the failed Authenticate() call
         * @return 401 or 403 response with content-type: application/json
         */
        [[nodiscard]]
        static boost::beast::http::response<boost::beast::http::string_body>
        Unauthorized(const boost::beast::http::request<boost::beast::http::string_body> &req, const AuthResult &auth);

        /**
         * @brief Validates the configured HS256 JWT signing secret (euclid.modules.eam.jwt-secret).
         *
         * Logs an error describing the problem and returns false if the secret is missing
         * (falling back to the insecure built-in default) or shorter than the 32-byte
         * minimum required for HMAC-SHA256. Intended to be called once at startup, before
         * the server accepts any requests.
         *
         * @return true if the configured secret is safe to use, false otherwise.
         */
        [[nodiscard]]
        static bool ValidateJwtSecret();

        /**
         * @brief Reads the configured HS256 JWT signing secret (euclid.modules.eam.jwt-secret),
         * falling back to the insecure built-in default if unconfigured.
         *
         * Exposed so modules that issue tokens (e.g. access, on login) can sign them with the
         * same secret Authenticate() verifies against.
         */
        [[nodiscard]]
        static std::string JwtSecret();

        /**
         * @brief Parses the request body as JSON.
         *
         * @param req request whose body is parsed
         * @param out set to the parsed value on success; left unspecified on failure
         * @return the 400 response to send back if the body isn't valid JSON, otherwise
         * std::nullopt (in which case @p out holds the parsed value).
         */
        [[nodiscard]]
        static std::optional<boost::beast::http::response<boost::beast::http::string_body> >
        ParseJsonBody(const boost::beast::http::request<boost::beast::http::string_body> &req, boost::json::value &out);

        /**
         * @brief Generates a request ID for use in a response body, e.g. AWS-style
         * "ResponseMetadata": {"RequestId": ...}.
         *
         * @return a freshly generated random UUID.
         */
        [[nodiscard]]
        static std::string RequestId();

        /**
         * @brief Builds the response for the "get-metrics" action every module accepts.
         *
         * Drains Core::Monitoring::MonitoringCollector::instance().Collect() (every metric this
         * process has recorded since the last call) into
         * {"items": [{"name", "labelName", "labelValue", "value", "type": "rate"|"gauge"}, ...]}.
         * Shared by every module's dispatch so metric collection/reporting isn't reimplemented
         * per module - see Core::Monitoring::MonitoringTimer for how a module records metrics in
         * the first place.
         *
         * @param req original request, used for the HTTP version and keep-alive flag
         * @return response with content-type: application/json
         */
        [[nodiscard]]
        static boost::beast::http::response<boost::beast::http::string_body>
        MetricsResponse(const boost::beast::http::request<boost::beast::http::string_body> &req);

    private:

        /**
         * @brief Id of the scheduled CPU/memory usage collection task, used to cancel it on destruction.
         */
        std::string _resourceUsageTaskId;
    };

}// namespace Euclid::Core