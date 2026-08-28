// C++ includes
#include <algorithm>
#include <charconv>
#include <optional>

// Boost includes
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>

// Mongo includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/replace.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/core/SigV4.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/manager/GatewayServer.h>

namespace Euclid::main {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    // Boost.Beast's parser defaults to a 1MB body limit when none is set explicitly, which is
    // silently exceeded by a single storage upload-part (default part size alone is 5MB) - the
    // parser then fails the read and the session tears down with no response, surfacing to the
    // client as a bare "connection reset". euclid.gateway.http.max-body already exists in the
    // shipped configs for this; it just wasn't wired to anything, so this is what makes it real.
    static std::uint64_t MaxBodySize() {
        constexpr long kDefaultMaxBodySize = 512L * 1024 * 1024;
        return static_cast<std::uint64_t>(Core::Configuration::instance().getOr<long>("euclid.gateway.http.max-body", kDefaultMaxBodySize));
    }

    // How long forwardToService() waits on the module before giving up. Matters most when the
    // autoscaler kills an instance mid-request (or any other backend hiccup): without a bound
    // here, that single request can wedge a gateway worker thread forever, and enough of those
    // pile up until the whole gateway - not just the module the stuck request happened to hit -
    // stops making progress on anything.
    static std::chrono::seconds BackendTimeout() {
        constexpr long kDefaultBackendTimeoutSeconds = 60;
        return std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.gateway.http.backend-timeout-seconds", kDefaultBackendTimeoutSeconds));
    }

    // ── Euclid service detection ────────────────────────────────────────────────

    // Returns the lowercase Euclid service name for a request, or empty string if
    // this is not an Euclid service call.
    //
    // Detection order:
    //   1. x-euclid-target — "DynamoDB_20120810.PutItem" → "dynamodb"
    //   2. Authorization   — SigV4 credential scope: "<key>/<date>/<region>/<svc>/aws4_request"
    static std::string detectEuclidService(const http::request<http::string_body> &req) {
        static const std::unordered_set<std::string> kModules{
                "eam", "esm", "eqs", "ens", "emm"
        };

        if (const auto module = std::string(req["x-euclid-target"]); !module.empty()) {
            log_debug << "Euclid service call detected from x-euclid-target: " << module;
            if (kModules.contains(module)) return module;
        }

        if (const auto auth = Core::SigV4::ParseAuthorizationHeader(std::string(req[http::field::authorization])); auth.has_value()) {
            log_debug << "Euclid service call detected from SigV4 credential scope: " << auth->scope.service;
            if (kModules.contains(auth->scope.service)) return auth->scope.service;
        }
        return {};
    }

    // Service:action pairs reachable without a bearer token - login has no token to present
    // yet, and register's own admin-vs-bootstrap check happens downstream in the access module
    // (it needs to know whether the user store is empty, which only that module can see).
    static bool isPublicAction(const std::string &service, const std::string &action) {
        static const std::unordered_set<std::string> kPublic{"eam:login", "eam:register"};
        return kPublic.contains(service + ":" + action);
    }

    // Proxies req over a Unix-domain socket at socketPath and returns the
    // backend's response.  Runs synchronously on the calling worker thread.
    static http::response<http::string_body> forwardToService(const http::request<http::string_body> &req, const std::string &socketPath) {
        namespace local = asio::local;
        auto errorResponse = [&](const http::status st, const std::string_view msg) {
            http::response<http::string_body> r{st, req.version()};
            r.set(http::field::content_type, "application/json");
            r.keep_alive(req.keep_alive());
            r.body() = boost::json::serialize(boost::json::object{{"error", msg}});
            r.prepare_payload();
            return r;
        };

        if (req.method() == http::verb::options) {
            http::response<http::string_body> r{http::status::no_content, req.version()};
            r.set(http::field::allow, "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            r.set("Access-Control-Allow-Origin", "*");
            r.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            r.set("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
            r.set("Access-Control-Max-Age", "86400");
            r.keep_alive(req.keep_alive());
            r.prepare_payload();
            return r;
        }

        // Chained as a single async pipeline (connect -> write -> read) under one ioc.run() so
        // BackendTimeout() actually bounds the whole exchange - beast::basic_stream's timeout
        // support, like tcp_stream's, only takes effect for asynchronous operations. The
        // synchronous connect/write/read this replaced ignored the timeout entirely and could
        // hang the calling gateway worker thread forever on a backend that never responds.
        try {
            asio::io_context ioc;
            beast::basic_stream<local::stream_protocol> stream(ioc);
            stream.expires_after(BackendTimeout());

            beast::error_code opEc;
            beast::flat_buffer buf;
            http::response<http::string_body> res;

            stream.async_connect(local::stream_protocol::endpoint(socketPath), [&](const beast::error_code &ec) {
                if (ec) {
                    opEc = ec;
                    return;
                }
                http::async_write(stream, req, [&](const beast::error_code &writeEc, std::size_t) {
                    if (writeEc) {
                        opEc = writeEc;
                        return;
                    }
                    http::async_read(stream, buf, res, [&](const beast::error_code &readEc, std::size_t) {
                        opEc = readEc;
                    });
                });
            });

            ioc.run();

            if (opEc) return errorResponse(http::status::bad_gateway, opEc.message());
            return res;
        } catch (const std::exception &ex) {
            return errorResponse(http::status::bad_gateway, ex.what());
        }
    }

    // ── "emm" (module manager) handling ────────────────────────────────────────
    // Unlike eam/esm/eqs/ens, "emm" isn't a separate module subprocess reachable over a Unix
    // socket - it's the manager itself, so its actions are answered directly here instead of
    // going through forwardToService(), reading straight from the module repository (Mongo or
    // in-memory, per euclid.database.backend - see RepositoryFactory) rather than proxying
    // anywhere.
    //
    // Every emm action is administrator-only (it exposes every module's live process pool -
    // pids, sockets, restart history), so this re-checks the caller's administrator-group
    // membership itself instead of trusting the CLI's own client-side check, which is a UX
    // nicety only and not a security boundary (nothing stops a modified/older client, or a
    // direct SigV4-signed curl, from skipping it). subject is the already-authenticated caller
    // from route() - always set, since "emm" is never in isPublicAction's allowlist.
    // Which of a module's own MongoDB collections "export" dumps. topLevel is what --full-less
    // export includes - the same resources euclid-cli's own list-* actions show (queues, topics,
    // buckets, users, ...). fullOnly is the bulk child data each of those resources owns
    // (EQS/ENS messages, ESM objects) - opt-in via --full, since it can dwarf the container
    // records around it and usually isn't what an operator wants for a quick inspection/backup.
    struct ModuleExportSpec {
        std::vector<std::string> topLevel;
        std::vector<std::string> fullOnly;
    };

    static const std::unordered_map<std::string, ModuleExportSpec> &moduleExportSpecs() {
        static const std::unordered_map<std::string, ModuleExportSpec> specs{
                {"eqs", {{"eqs_queue"}, {"eqs_message"}}},
                {"ens", {{"ens_topic", "ens_subscription"}, {"ens_message"}}},
                {"esm", {{"esm_bucket", "esm_subscription"}, {"esm_object"}}},
                {"eam", {{"eam_user", "eam_usergroup", "eam_account", "eam_namespace"}, {}}},
                {"emm", {{"emm_module"}, {}}},
                {"emo", {{"emo_data"}, {}}},
        };
        return specs;
    }

    // Renders one collection as a JSON array of its documents. Each document round-trips through
    // bsoncxx's relaxed Extended JSON (ISO dates, plain numbers, {"$oid": ...} for ObjectIds) on
    // the way to a boost::json::value, rather than being hand-assembled from known fields - export
    // is meant to reflect whatever is actually stored, including fields no repository method
    // happens to read back out.
    static boost::json::array exportCollection(mongocxx::collection collection) {
        boost::json::array docs;
        for (auto cursor = collection.find({}); const auto &doc: cursor) {
            docs.push_back(boost::json::parse(bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_relaxed)));
        }
        return docs;
    }

    // Collection name -> owning module, derived from moduleExportSpecs() (the single source of
    // truth for which collections belong to a module). "import" uses this to reject any collection
    // name it doesn't recognize instead of blindly writing wherever an import file happens to name
    // - the file is client-supplied input crossing a trust boundary, not something the server
    // generated itself the way an export file normally would be.
    static const std::unordered_map<std::string, std::string> &collectionModules() {
        static const std::unordered_map<std::string, std::string> result = [] {
            std::unordered_map<std::string, std::string> m;
            for (const auto &[module, spec]: moduleExportSpecs()) {
                for (const auto &c: spec.topLevel) m[c] = module;
                for (const auto &c: spec.fullOnly) m[c] = module;
            }
            return m;
        }();
        return result;
    }

    // Upserts one collection's worth of documents (by "_id", replacing the whole document rather
    // than merging fields, so a restore reflects the export exactly - including fields since
    // removed from the live document). Each document is parsed independently so one malformed
    // entry - the file is external input, possibly hand-edited - fails on its own instead of
    // aborting the rest of the collection.
    static std::pair<long, long> importCollection(mongocxx::collection collection, const boost::json::array &docs) {
        namespace basic = bsoncxx::builder::basic;
        long imported = 0;
        long failed = 0;
        mongocxx::options::replace opts;
        opts.upsert(true);
        for (const auto &doc: docs) {
            try {
                const auto bsonDoc = bsoncxx::from_json(boost::json::serialize(doc));
                const auto idElement = bsonDoc.view()["_id"];
                if (!idElement) {
                    ++failed;
                    continue;
                }
                collection.replace_one(basic::make_document(basic::kvp("_id", idElement.get_value())), bsonDoc.view(), opts);
                ++imported;
            } catch (const std::exception &e) {
                log_warning << "emm import: skipping malformed document, collection: " << std::string(collection.name()) << ", error: " << e.what();
                ++failed;
            }
        }
        return {imported, failed};
    }

    static http::response<http::string_body> handleEmmRequest(const http::request<http::string_body> &req, const std::string &subject) {
        auto jsonResponse = [&](const http::status status, std::string body) {
            http::response<http::string_body> r{status, req.version()};
            r.set(http::field::content_type, "application/json");
            r.keep_alive(req.keep_alive());
            r.body() = std::move(body);
            r.prepare_payload();
            return r;
        };
        auto err = [&](const http::status status, const std::string &msg) {
            return jsonResponse(status, boost::json::serialize(boost::json::object{{"error", msg}}));
        };

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto caller = repo->findUserByUserId(subject);
        if (!caller.has_value() || !Database::IsEamAdmin(*repo, caller->userId)) {
            return err(http::status::forbidden, "administrator privileges required");
        }

        const auto action = std::string(req["x-euclid-action"]);

        if (action == "export") {
            boost::json::value jv = boost::json::object{};
            if (!req.body().empty()) {
                try {
                    jv = boost::json::parse(req.body());
                } catch (const std::exception &) {
                    return err(http::status::bad_request, "invalid JSON body");
                }
            }
            const auto all = Core::GetBoolValue(jv, "all");
            const auto requestedModules = Core::GetStringArrayValue(jv, "modules");
            const auto full = Core::GetBoolValue(jv, "full");

            if (all == !requestedModules.empty()) {
                return err(http::status::bad_request, "exactly one of \"all\" or \"modules\" is required");
            }

            const auto &specs = moduleExportSpecs();
            std::vector<std::string> modules;
            if (all) {
                for (const auto &[name, spec]: specs) modules.push_back(name);
                std::ranges::sort(modules);
            } else {
                for (const auto &module: requestedModules) {
                    if (!specs.contains(module)) {
                        return err(http::status::bad_request, "unknown module '" + module + "' (expected one of: eam, emm, emo, ens, eqs, esm)");
                    }
                    modules.push_back(module);
                }
            }

            const auto entry = Database::Database::instance().client();
            auto db = (*entry)[Database::Database::instance().databaseName()];

            boost::json::object collections;
            for (const auto &module: modules) {
                const auto &spec = specs.at(module);
                for (const auto &collectionName: spec.topLevel) {
                    collections[collectionName] = exportCollection(db[collectionName]);
                }
                if (full) {
                    for (const auto &collectionName: spec.fullOnly) {
                        collections[collectionName] = exportCollection(db[collectionName]);
                    }
                }
            }

            boost::json::array modulesJson(modules.begin(), modules.end());
            return jsonResponse(http::status::ok, boost::json::serialize(boost::json::object{
                                        {"modules", std::move(modulesJson)},
                                        {"full", full},
                                        {"exportedAt", Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())},
                                        {"collections", std::move(collections)},
                                }));
        }

        if (action == "import") {
            if (req.body().empty()) {
                return err(http::status::bad_request, "missing JSON body");
            }
            boost::json::value jv;
            try {
                jv = boost::json::parse(req.body());
            } catch (const std::exception &) {
                return err(http::status::bad_request, "invalid JSON body");
            }
            if (!jv.is_object() || !Core::AttributeExists(jv, "collections") || !jv.at("collections").is_object()) {
                return err(http::status::bad_request, "missing \"collections\" object");
            }

            // Optional filter, e.g. re-importing only "esm" out of a multi-module export file;
            // an empty filter (the common case - re-importing everything the file contains) means
            // no restriction.
            const auto moduleFilter = Core::GetStringArrayValue(jv, "modules");
            const std::unordered_set<std::string> allowedModules(moduleFilter.begin(), moduleFilter.end());

            const auto &collModules = collectionModules();
            const auto entry = Database::Database::instance().client();
            auto db = (*entry)[Database::Database::instance().databaseName()];

            boost::json::object imported;
            boost::json::array skipped;
            for (const auto &[collectionName, docsValue]: jv.at("collections").as_object()) {
                const std::string name(collectionName);
                const auto owner = collModules.find(name);
                if (owner == collModules.end()) {
                    skipped.push_back(boost::json::object{{"collection", name}, {"reason", "unknown collection"}});
                    continue;
                }
                if (!allowedModules.empty() && !allowedModules.contains(owner->second)) {
                    skipped.push_back(boost::json::object{{"collection", name}, {"reason", "excluded by \"modules\" filter"}});
                    continue;
                }
                if (!docsValue.is_array()) {
                    skipped.push_back(boost::json::object{{"collection", name}, {"reason", "not a JSON array"}});
                    continue;
                }

                const auto [count, failed] = importCollection(db[name], docsValue.as_array());
                boost::json::object result{{"imported", count}};
                if (failed > 0) result["failed"] = failed;
                imported[name] = std::move(result);
            }

            return jsonResponse(http::status::ok, boost::json::serialize(boost::json::object{
                                        {"imported", std::move(imported)},
                                        {"skipped", std::move(skipped)},
                                }));
        }

        if (action != "list-modules") {
            return err(http::status::not_found, "unknown emm action '" + action + "'");
        }

        boost::json::array modules;
        for (const auto &m: Database::RepositoryFactory::instance().emmRepository()->findAll()) {
            boost::json::array instances;
            for (const auto &i: m.instances) {
                instances.push_back(boost::json::object{
                        {"instanceId", i.instanceId},
                        {"pid", i.pid},
                        {"state", Database::Entity::ModuleStateToString(i.state)},
                        {"socketPath", i.socketPath},
                        {"restartCount", i.restartCount},
                        {"created", Core::DateTimeUtils::ToISO8601(i.created)},
                        {"modified", Core::DateTimeUtils::ToISO8601(i.modified)},
                });
            }
            modules.push_back(boost::json::object{
                    {"name", m.name},
                    {"executable", m.executable},
                    {"socketPath", m.socketPath},
                    {"active", m.active},
                    {"autoRestart", m.autoRestart},
                    {"maxRestarts", m.maxRestarts},
                    {"created", Core::DateTimeUtils::ToISO8601(m.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(m.modified)},
                    {"lastStartTime", m.lastStartTime.time_since_epoch().count() == 0 ? boost::json::value(nullptr) : boost::json::value(Core::DateTimeUtils::ToISO8601(m.lastStartTime))},
                    {"instances", std::move(instances)},
            });
        }
        const auto total = static_cast<long>(modules.size());
        return jsonResponse(http::status::ok, boost::json::serialize(boost::json::object{
                                    {"modules", std::move(modules)},
                                    {"total", total},
                            }));
    }

    // ── Router ───────────────────────────────────────────────────────────────
    // Shared by both GatewaySession and GatewayTlsSession - routing doesn't depend
    // on the underlying stream type.

    static http::response<http::string_body> route(const http::request<http::string_body> &req, ServiceController &ctrl) {

        const auto version = req.version();
        const auto keepAlive = req.keep_alive();

        // Strip query string from target
        std::string target(req.target());
        if (const auto q = target.find('?'); q != std::string::npos) target.resize(q);

        auto jsonResponse = [&](const http::status status, std::string body) {
            http::response<http::string_body> r{status, version};
            r.set(http::field::content_type, "application/json");
            r.keep_alive(keepAlive);
            r.body() = std::move(body);
            r.prepare_payload();
            return r;
        };

        auto ok = [&](std::string body) {
            return jsonResponse(http::status::ok, std::move(body));
        };
        auto err = [&](const http::status status, std::string_view msg) {
            return jsonResponse(status, boost::json::serialize(boost::json::object{{"error", msg}}));
        };

        // A single unhandled exception anywhere below (auth, JSON parsing, forwarding, ...) must
        // not escape this function: it would propagate out of the async_read completion handler
        // that called route(), out of the io_context::run() on whichever gateway worker thread
        // happened to be running it, and - since an exception escaping a std::thread's entry
        // function calls std::terminate() - take down the entire manager process, every module it
        // manages included. Converting it into a 500 here costs one failed request instead of the
        // whole gateway. Mirrors the same guard UnixSocketServer::Session::doRead() already has
        // around Dispatch() for individual module processes.
        try {
            // ── Euclid service dispatch ─────────────────────────────────────────
            if (const auto service = detectEuclidService(req); !service.empty()) {
                const auto action = std::string(req["x-euclid-action"]);
                std::optional<std::string> subject;
                if (!isPublicAction(service, action)) {
                    const auto auth = Core::HttpActionServer::Authenticate(req);
                    if (!auth.subject.has_value()) {
                        return Core::HttpActionServer::Unauthorized(req, auth);
                    }
                    subject = auth.subject;
                }

                // "emm" is never public (not in isPublicAction's allowlist), so subject is
                // always set here - handleEmmRequest() re-checks the caller is an admin itself.
                if (service == "emm") {
                    return handleEmmRequest(req, *subject);
                }

                // Lets a client proactively declare concurrency it's about to need (e.g. the
                // storage CLI's upload-file --concurrency, sent on create-upload) so the autoscaler
                // can ramp toward it directly instead of waiting to sample busy instances - see
                // ServiceController::declareExpectedConcurrency()'s doc comment. Generic at the
                // gateway level (any module's requests can carry it), not storage-specific.
                if (const auto concurrency = req["x-euclid-expected-concurrency"]; !concurrency.empty()) {
                    if (int desired = 0; std::from_chars(concurrency.data(), concurrency.data() + concurrency.size(), desired).ec == std::errc{} && desired > 0) {
                        if (!ctrl.declareExpectedConcurrency(service, desired)) {
                            return err(http::status::bad_request, "requested concurrency (" + std::to_string(desired) + ") exceeds configured max instances for service '" + service + "'");
                        }
                    }
                }

                const auto handle = ctrl.acquireInstance(service);
                if (!handle) return err(http::status::service_unavailable, "service '" + service + "' not registered or not running");

                // Guarantees the autoscaler's active-request count is released on every exit path
                // below, however forwardToService() returns.
                struct ReleaseGuard {
                    ServiceController &ctrl;
                    const std::string &name;
                    pid_t pid;
                    ~ReleaseGuard() { ctrl.releaseInstance(name, pid); }
                } guard{.ctrl = ctrl, .name = service, .pid = handle->pid};

                return forwardToService(req, handle->socketPath);
            }

            return err(http::status::not_found, "not found");
        } catch (const std::exception &ex) {
            log_error << "route() threw, error: " << ex.what();
            return err(http::status::internal_server_error, ex.what());
        } catch (...) {
            log_error << "route() threw a non-std::exception";
            return err(http::status::internal_server_error, "internal server error");
        }
    }

    // ── GatewaySession ───────────────────────────────────────────────────────
    // Handles one plaintext TCP connection: reads requests in a loop (keep-alive
    // aware), dispatches to the router, and writes responses.

    class GatewaySession : public std::enable_shared_from_this<GatewaySession> {
    public:

        GatewaySession(tcp::socket socket, ServiceController &ctrl) : _stream(std::move(socket)), _ctrl(ctrl) {}

        void run() { doRead(); }

    private:

        void doRead() {
            _parser.emplace();
            _parser->body_limit(MaxBodySize());
            _stream.expires_after(std::chrono::seconds(30));
            http::async_read(
                    _stream, _buf, *_parser,
                    [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                        if (!ec) self->handleRequest();
                        // on error (EOF, timeout, body_limit exceeded, etc.) the session destructs naturally
                    });
        }

        void handleRequest() {
            auto res = route(_parser->get(), _ctrl);
            auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
            const bool keepAlive = sp->keep_alive();
            http::async_write(
                    _stream, *sp,
                    [self = shared_from_this(), sp, keepAlive](const beast::error_code &ec, std::size_t) {
                        if (!ec && keepAlive) self->doRead();
                        else {
                            beast::error_code ignored;
                            std::ignore = self->_stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
                        }
                    });
        }

        beast::tcp_stream _stream;
        beast::flat_buffer _buf;
        std::optional<http::request_parser<http::string_body> > _parser;
        ServiceController &_ctrl;
    };

    // ── GatewayTlsSession ────────────────────────────────────────────────────
    // Same request loop as GatewaySession, but the connection is terminated with
    // TLS: a handshake runs before the first read, and the stream is an SSL layer
    // wrapped around the TCP stream.

    class GatewayTlsSession : public std::enable_shared_from_this<GatewayTlsSession> {
    public:

        GatewayTlsSession(tcp::socket socket, asio::ssl::context &sslCtx, ServiceController &ctrl)
            : _stream(std::move(socket), sslCtx), _ctrl(ctrl) {}

        void run() {
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
            _stream.async_handshake(
                    asio::ssl::stream_base::server,
                    [self = shared_from_this()](const beast::error_code &ec) {
                        if (!ec) self->doRead();
                        // handshake failure: session destructs naturally
                    });
        }

    private:

        void doRead() {
            _parser.emplace();
            _parser->body_limit(MaxBodySize());
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
            http::async_read(
                    _stream, _buf, *_parser,
                    [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                        if (!ec) self->handleRequest();
                        // on error (EOF, timeout, body_limit exceeded, etc.) the session destructs naturally
                    });
        }

        void handleRequest() {
            auto res = route(_parser->get(), _ctrl);
            auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
            const bool keepAlive = sp->keep_alive();
            http::async_write(
                    _stream, *sp,
                    [self = shared_from_this(), sp, keepAlive](const beast::error_code &ec, std::size_t) {
                        if (!ec && keepAlive) self->doRead();
                        else {
                            // Deliberately closes the raw socket rather than calling the
                            // stream's shutdown() (SSL "close_notify" handshake): that overload
                            // is *synchronous* and, unlike every other operation here, isn't
                            // covered by expires_after() - it blocks waiting for the peer to
                            // send its own close_notify back, and a client that's already done
                            // with the connection (e.g. one that never reuses connections, like
                            // euclid-cli) has no reason to send one. Since this runs on a
                            // gateway worker thread shared by every other in-flight connection
                            // (see BackendTimeout()'s doc comment on the same hazard for
                            // forwardToService()), a single hung shutdown() here would stall the
                            // whole gateway, not just this session.
                            beast::get_lowest_layer(self->_stream).close();
                        }
                    });
        }

        beast::ssl_stream<beast::tcp_stream> _stream;
        beast::flat_buffer _buf;
        std::optional<http::request_parser<http::string_body> > _parser;
        ServiceController &_ctrl;
    };

    // ── GatewayServer ────────────────────────────────────────────────────────

    GatewayServer::GatewayServer(ServiceController &ctrl, const unsigned short port, const int threads) : _ctrl(ctrl), _ioc(threads), _acceptor(_ioc), _port(port), _threads(threads) {

        const tcp::endpoint endpoint{tcp::v6(), port};
        _acceptor.open(endpoint.protocol());
        _acceptor.set_option(asio::socket_base::reuse_address(true));
        _acceptor.set_option(asio::ip::v6_only(false));
        _acceptor.bind(endpoint);
        _acceptor.listen(asio::socket_base::max_listen_connections);

        _tlsEnabled = Core::Configuration::instance().getOr<bool>("euclid.gateway.tls.enabled", false);
        if (_tlsEnabled) {
            const auto certFile = Core::Configuration::instance().getOr<std::string>("euclid.gateway.tls.cert-file", "");
            const auto keyFile = Core::Configuration::instance().getOr<std::string>("euclid.gateway.tls.key-file", "");
            if (certFile.empty() || keyFile.empty()) {
                throw std::runtime_error("euclid.gateway.tls.enabled is true but cert-file/key-file are not configured");
            }
            _sslCtx.use_certificate_chain_file(certFile);
            _sslCtx.use_private_key_file(keyFile, asio::ssl::context::pem);
        }
    }

    GatewayServer::~GatewayServer() { stop(); }

    void GatewayServer::start() {
        doAccept();
        _workers.reserve(_threads);
        for (int i = 0; i < _threads; ++i) _workers.emplace_back([this] { _ioc.run(); });
        log_info << "Gateway " << (_tlsEnabled ? "HTTPS" : "HTTP") << " server listening on port " << _port << " (" << _threads << " worker thread(s))";
    }

    void GatewayServer::stop() {
        _acceptor.close();
        _ioc.stop();
        for (auto &t: _workers) if (t.joinable()) t.join();
        _workers.clear();
    }

    void GatewayServer::doAccept() {
        _acceptor.async_accept(asio::make_strand(_ioc), [this](const beast::error_code &ec, tcp::socket socket) {
            if (!ec) {
                if (_tlsEnabled) std::make_shared<GatewayTlsSession>(std::move(socket), _sslCtx, _ctrl)->run();
                else std::make_shared<GatewaySession>(std::move(socket), _ctrl)->run();
            }
            if (!_ioc.stopped()) doAccept();
        });
    }

}// namespace Euclid::main