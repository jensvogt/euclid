// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
     * @author jensvogt47\@gmail.com
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
         * @brief One command, as the audit trail records it.
         *
         * @par
         * A plain struct rather than the database entity, because core cannot depend on database -
         * the dependency runs the other way. The sink that is handed one of these turns it into a
         * row; see Database::WireAuditSink().
         */
        struct AuditRecord {

            /**
             * @brief Account the command was run in.
             */
            std::string accountId;

            /**
             * @brief Namespace it was run in, empty for a command that names none.
             */
            std::string nameSpace;

            /**
             * @brief The verified caller, not what the body claimed.
             */
            std::string userId;

            /**
             * @brief Module the command was addressed to.
             */
            std::string moduleName;

            /**
             * @brief The command.
             */
            std::string command;

            /**
             * @brief The request body, for the sink to redact before it stores it.
             */
            std::string parameters;

            /**
             * @brief The HTTP status it was answered with.
             */
            long status{};
        };

        /**
         * @brief Callback the audit trail is written through.
         */
        using AuditSink = std::function<void(const AuditRecord &)>;

        /**
         * @brief Registers where audited commands go.
         *
         * @par Why here rather than in each module
         * Dispatch() is the one place every action of every module passes through, already holding
         * the caller identity, the target, the action and the body. Auditing anywhere else means
         * remembering it in forty handlers, and the ones nobody remembered would be the ones
         * missing from the trail - which is the failure an audit cannot tolerate, because absence
         * of a record reads as absence of the command.
         *
         * @par What is recorded
         * Everything that changes something, and everything that was refused or failed whatever
         * its kind. Successful reads are left out by default: on a working installation they are
         * the overwhelming majority - a single parse run makes millions of esm:get-object calls -
         * and a trail that large buries what it was kept for. `euclid.modules.ead.audit-reads`
         * turns them on for an installation that wants them.
         *
         * @par
         * Until this is called nothing is recorded, which is what a tool built on this class that
         * is not a module keeps doing.
         *
         * @param sink invoked with each audited command; must not throw, and must not block.
         */
        static void SetAuditSink(AuditSink sink);

        /**
         * @brief Whether one answered command belongs in the audit trail.
         *
         * @par
         * Public so it can be tested. The rule decides what an audit does and does not contain,
         * which is not something to leave pinned only by the comment explaining it - an exclusion
         * whose reasoning is tested but whose behaviour is not will pass while it is being
         * silently reverted, which is how this came to be public.
         *
         * @par What is in
         * Everything that changes a resource somebody manages - a queue, a topic, a bucket, a key,
         * a secret, a table, a user, an application - and everything that was refused or failed
         * whatever its kind. A refusal is the entry an audit exists for, and a 403 that is not
         * recorded looks exactly like a command nobody attempted. Successful reads only when
         * `euclid.modules.ead.audit-reads` asks for them.
         *
         * @par What is never in
         * What flows through those resources rather than being one of them: messages, objects,
         * items, events and parts, when the command succeeded. That is the traffic, and it arrives
         * at a rate no audit can hold - see Permissions::IsSecondLevel for the rule and the
         * measurements behind it. What brackets such traffic is kept, so an upload is still a
         * recorded event even though its 1,479 parts are not.
         *
         * @par
         * Also never in: the machinery modules and the machinery actions, whatever the status -
         * see the implementation for which and why.
         *
         * @param target the module addressed, from x-euclid-target.
         * @param action the command, from x-euclid-action.
         * @param status the status it was answered with.
         * @return true when it should be recorded.
         */
        static bool ShouldAudit(std::string_view target, std::string_view action, long status);

    protected:

        /**
         * @brief Hands one answered command to the audit sink, if it is one worth recording.
         *
         * @param req the request as it arrived.
         * @param status the status it was answered with.
         */
        static void RecordAudit(const boost::beast::http::request<boost::beast::http::string_body> &req, long status);

    public:

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
         * @brief Refuses to start when the configuration still asks for a mode that no longer
         * exists.
         *
         * @par
         * `euclid.authorization.mode` had three values while euclid was moving off the per-user
         * grant lists. Those lists are gone, so "legacy" and "shadow" - both of which meant "let the
         * old mechanism decide" - would now mean "let nothing decide". A module that found one of
         * them in its configuration and started anyway would authorize nobody and say nothing.
         *
         * @par
         * Called from this class's constructor, so every module gets it without opting in. Loud at
         * deploy time is the worst this can be; silent and open is what it prevents.
         *
         * @throws std::runtime_error if the setting is present and is not "enforce".
         */
        static void RequireEnforcingAuthorization();

        /**
         * @brief Whether a caller may do what a request asks, and why.
         */
        struct AuthorizationDecision {
            bool allowed{false};
            std::string reason;
        };

        /**
         * @brief Answers whether a request is authorized.
         *
         * @par
         * Registered rather than called directly, for the reason SetAccessKeyLookup() exists: this
         * class is in core, and roles, grants and users are in the database module that depends on
         * it. A process that never registers one is never refused anything - which is what keeps
         * this change additive until somebody turns it on.
         *
         * @param req    the request, for its identity headers.
         * @param target module target, from x-euclid-target.
         * @param action module action, from x-euclid-action.
         */
        using AuthorizationLookup = std::function<AuthorizationDecision(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                                                        const std::string &target, const std::string &action)>;

        /**
         * @brief Registers the lookup the gate consults. See Database::WireAuthorizationLookup().
         *
         * @param lookup answers the decision, or nothing to leave the gate inert.
         */
        static void SetAuthorizationLookup(AuthorizationLookup lookup);


        /**
         * @brief The refusal this request earns from the role gate, or nothing to let it through.
         *
         * @par
         * What Dispatch() calls before the module's own handler, and public for the same reason
         * the resource check next door is: it is a pure function of the request and the registered
         * lookup, and it is the single decision this whole design turns on. A gate that could only
         * be exercised by starting a server would be tested by nobody.
         *
         * @par
         * Answers nothing - let it through - in the two cases that are not a considered refusal: no
         * registered lookup, or a module whose actions no role can name.
         *
         * @param req the request.
         * @return the 403 to send instead, or std::nullopt.
         */
        [[nodiscard]]
        static std::optional<boost::beast::http::response<boost::beast::http::string_body> >
        Authorize(const boost::beast::http::request<boost::beast::http::string_body> &req);


        /**
         * @brief Whether a caller may act on one named resource, and why.
         *
         * @par
         * The second half of the gate, and the half that cannot live in it. The module and the
         * action are headers, so Authorize() settles them before any handler runs; the *resource*
         * is a bucket ERN in a body or a queue ERN in a header, and only the handler knows which
         * field holds it. So the handler asks, once it has read one - see
         * docs/role-concept.md §4.1.
         *
         * @param req         the request, for its identity headers.
         * @param target      module target.
         * @param action      module action.
         * @param resourceErn the resource the handler has just read out of the request.
         */
        using ResourceAuthorizationLookup = std::function<AuthorizationDecision(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                                                                const std::string &target, const std::string &action,
                                                                                const std::string &resourceErn)>;

        /**
         * @brief Registers the lookup AuthorizeResource() consults.
         */
        static void SetResourceAuthorizationLookup(ResourceAuthorizationLookup lookup);

        /**
         * @brief The refusal a caller earns for naming this resource, or nothing to let it through.
         *
         * @par
         * Called by a handler once it has read the resource the request is about.
         *
         * @param req         the request.
         * @param resourceErn the resource it names.
         * @return the 403 to send instead, or std::nullopt.
         */
        [[nodiscard]]
        static std::optional<boost::beast::http::response<boost::beast::http::string_body> >
        AuthorizeResource(const boost::beast::http::request<boost::beast::http::string_body> &req,
                          const std::string &resourceErn);

        /**
         * @brief Whether a caller may act on one named resource.
         *
         * @par
         * The same question AuthorizeResource() asks, answered as a boolean for the callers that
         * *filter* rather than refuse - a listing that quietly leaves out what the caller may not
         * reach, so an application asking what it can read gets an answer instead of a 403.
         *
         * @param req         the request.
         * @param resourceErn the resource.
         * @return true if the caller may act on it, and true when no lookup is registered.
         */
        [[nodiscard]]
        static bool IsResourceAuthorized(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                         const std::string &resourceErn);

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
         * @brief Resolves an access key ID through the registered lookup.
         *
         * @par
         * What Authenticate() does internally, for the one caller that has to verify a signature
         * itself rather than letting Authenticate() do it: the API gateway, whose upload routes
         * check the signature before the body has arrived and so cannot use a verifier that
         * compares the body to the digest. It resolves nothing a process that has called
         * SetAccessKeyLookup() could not already resolve by asking Authenticate() to verify
         * something.
         *
         * @param accessKeyId the key to resolve.
         * @return its secret and owner, or std::nullopt if unknown or no lookup is registered.
         */
        [[nodiscard]]
        static std::optional<AccessKeyRecord> LookupAccessKey(const std::string &accessKeyId);

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

    protected:

        /**
         * @brief Routes a request to its handler.
         *
         * @par
         * What every module implements, and what Dispatch() calls once a request has got past the
         * role gate. Renamed from Dispatch() when the gate arrived: a module that implemented
         * Dispatch() would have replaced the gate rather than sat behind it, and the failure would
         * have been one module silently ungated.
         *
         * @param req request received on the Unix domain socket.
         * @return the response to send back.
         */
        [[nodiscard]]
        virtual boost::beast::http::response<boost::beast::http::string_body>
        DispatchAction(const boost::beast::http::request<boost::beast::http::string_body> &req) = 0;

        /**
         * @brief The role gate, then the module's own handler.
         *
         * @par
         * final, deliberately. This is the one place every module's requests pass through, and the
         * whole point of gating here rather than in each handler is that a module cannot be left
         * out by forgetting - see docs/role-concept.md §4.1.
         */
        [[nodiscard]]
        boost::beast::http::response<boost::beast::http::string_body>
        Dispatch(const boost::beast::http::request<boost::beast::http::string_body> &req) final;

    };

}// namespace Euclid::Core