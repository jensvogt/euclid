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