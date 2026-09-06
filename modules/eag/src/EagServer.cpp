//
// Created by vogje01 on 9/5/26.
//

// C++ includes
#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Euclid includes
#include <EagServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/entity/eam/User.h>

namespace Euclid::EAG {

    using Database::Entity::EAG::Route;
    using Database::Entity::EAG::RouteAuthentication;
    using Database::Entity::EAG::RouteAuthenticationFromString;
    using Database::Entity::EAG::RouteAuthenticationToString;

    namespace {

        constexpr auto kServiceTimer = "eag-service-time";
        constexpr auto kServiceCounter = "eag-service-count";

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        std::string stringField(const boost::json::object &obj, const std::string &key, const std::string &fallback = "") {
            const auto *value = obj.if_contains(key);
            return value && value->is_string() ? std::string(value->as_string()) : fallback;
        }

        // The euclid modules a route may name. Kept as a list so a typo is refused at configuration
        // time: a route naming "emm " or "eeam" would otherwise be accepted, published, and answer
        // 404 from the gateway forever with nothing saying why.
        const std::set<std::string> kModuleTargets{"eam", "esm", "eqs", "ens", "emm", "emo", "ekm", "ets", "eap", "ees", "eag"};

        // The methods a route may name. A typo here is a route that quietly answers for nothing,
        // or - worse, on an update - one that stops answering for what it used to.
        const std::set<std::string> kKnownMethods{"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};

        // Reads and normalises the method list. Upper-cased because HTTP methods are
        // case-sensitive on the wire and always upper case there, while "get" is what somebody
        // types. Returns nothing if a name is not a method, so a typo is refused rather than
        // stored.
        std::optional<std::vector<std::string> > readMethods(const boost::json::object &obj, std::string &unknown) {

            std::vector<std::string> methods;
            const auto *value = obj.if_contains("methods");
            if (!value || !value->is_array()) return methods;

            for (const auto &entry: value->as_array()) {
                if (!entry.is_string()) {
                    unknown = boost::json::serialize(entry);
                    return std::nullopt;
                }
                auto method = std::string(entry.as_string());
                std::ranges::transform(method, method.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });

                if (!kKnownMethods.contains(method)) {
                    unknown = method;
                    return std::nullopt;
                }
                if (std::ranges::find(methods, method) == methods.end()) methods.push_back(method);
            }
            return methods;
        }

        // Whether another route already answers for this path *and* one of these methods.
        //
        // Two routes may share a path as long as their methods do not overlap - that is what lets
        // reads and writes of one resource go to different applications. What cannot be allowed is
        // an overlap, where the winner would be decided by whichever the sort happened to put
        // first: the caller would reach one application today and possibly the other tomorrow.
        // An empty method list means every method, so it overlaps with everything.
        std::optional<std::string> pathTaken(const std::string &path, const std::vector<std::string> &methods,
                                             const std::string &nameSpace, const std::string &routeId) {
            for (const auto &route: Database::RepositoryFactory::instance().eagRepository()->listRoutes(path)) {
                if (route.path != path || route.routeId == routeId) continue;

                // Scoped to the namespace, because a listener only ever sees its own: development
                // and integration can both publish "/api/produktmeldungen" on their own ports
                // without either being ambiguous. Only routes naming no namespace clash with
                // everything, since a listener carries those too whatever it is bound to.
                if (!nameSpace.empty() && !route.nameSpace.empty() && route.nameSpace != nameSpace) continue;
                if (route.methods.empty() || methods.empty()) return route.routeId;

                for (const auto &method: methods) {
                    if (std::ranges::find(route.methods, method) != route.methods.end()) return route.routeId;
                }
            }
            return std::nullopt;
        }

        boost::json::object toJson(const Route &route) {
            return {
                    {"routeId", route.routeId},
                    {"ern", route.ern},
                    {"accountId", route.accountId},
                    {"region", route.region},
                    {"namespace", route.nameSpace},
                    {"path", route.path},
                    {"applicationId", route.applicationId},
                    {"moduleTarget", route.moduleTarget},
                    {"moduleAction", route.moduleAction},
                    {"methods", boost::json::array(route.methods.begin(), route.methods.end())},
                    {"authentication", RouteAuthenticationToString(route.authentication)},
                    {"active", route.active},
                    {"created", Core::DateTimeUtils::ToISO8601(route.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(route.modified)}};
        }

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EagServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EagServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Every action here decides what the gateway exposes to the outside world, and on what terms -
    // which path reaches which application, and whether a caller needs a credential at all. That
    // is administrator work.
    static std::optional<response<string_body> > requireAdmin(const request<string_body> &req, AuthResult &auth) {
        auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), auth.user->userId)) {
            return EagServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }
        return std::nullopt;
    }

    // ── Action handlers ──────────────────────────────────────────────────────

    static response<string_body> handleCreateRoute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-route");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EagServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EagServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto routeId = stringField(obj, "routeId");
        const auto path = stringField(obj, "path");
        const auto applicationId = stringField(obj, "applicationId");
        const auto moduleTarget = stringField(obj, "moduleTarget");
        const auto moduleAction = stringField(obj, "moduleAction");
        if (routeId.empty()) return EagServer::ErrorResponse(req, status::bad_request, "routeId is required");

        // One or the other, never both and never neither: a route goes to an application euclid
        // runs, or to euclid itself, and those are reached in entirely different ways.
        if (applicationId.empty() == moduleTarget.empty()) {
            return EagServer::ErrorResponse(req, status::bad_request,
                                            "Name either an application or a euclid module, not both and not neither");
        }
        if (!moduleTarget.empty()) {
            if (!kModuleTargets.contains(moduleTarget)) {
                return EagServer::ErrorResponse(req, status::bad_request, "Not a euclid module: " + moduleTarget);
            }
            // Without an action the gateway has nothing to dispatch on and would answer 400 for
            // every request the route ever carries.
            if (moduleAction.empty()) {
                return EagServer::ErrorResponse(req, status::bad_request, "moduleAction is required when a module is named");
            }
        }

        // A path that does not start with "/" would never match anything, since what is compared
        // against it is a request target - refused here rather than becoming a route that is
        // configured, listed, and silently dead.
        if (path.empty() || !path.starts_with("/")) {
            return EagServer::ErrorResponse(req, status::bad_request, "path is required and must start with '/'");
        }

        std::string unknownMethod;
        const auto methods = readMethods(obj, unknownMethod);
        if (!methods.has_value()) {
            return EagServer::ErrorResponse(req, status::bad_request, "Not an HTTP method: " + unknownMethod);
        }

        const auto repository = Database::RepositoryFactory::instance().eagRepository();
        if (repository->routeExists(routeId)) {
            return EagServer::ErrorResponse(req, status::conflict, "Route exists already, routeId: " + routeId);
        }
        const auto nameSpace = stringField(obj, "namespace", std::string(req["x-euclid-namespace"]));
        if (const auto taken = pathTaken(path, *methods, nameSpace, routeId)) {
            return EagServer::ErrorResponse(req, status::conflict, "Path and method are already routed by routeId: " + *taken);
        }

        // The application has to exist. A route to nothing answers 503 for every request, which
        // looks like an application that is down rather than one that was never deployed.
        if (!applicationId.empty()
            && !Database::RepositoryFactory::instance().eapRepository()->findApplicationByApplicationId(applicationId).has_value()) {
            return EagServer::ErrorResponse(req, status::not_found, "Application not found, applicationId: " + applicationId);
        }

        Route route;
        route.routeId = routeId;
        route.path = path;
        route.applicationId = applicationId;
        route.moduleTarget = moduleTarget;
        route.moduleAction = moduleAction;
        route.methods = *methods;
        route.accountId = auth.user->accountId;

        // The scope requests carried by this route act in, which the gateway puts on each one
        // before it goes anywhere. Defaulted to whoever is creating the route, because that is
        // almost always what is meant, and nameable because it is not always: a route published
        // for one namespace should not act in another just because an administrator of the first
        // happened to configure it.
        route.region = stringField(obj, "region", auth.user->region);
        route.nameSpace = nameSpace;
        route.ern = Core::createErn("eag", route.accountId, route.nameSpace, "route:" + routeId);
        const auto authentication = RouteAuthenticationFromString(stringField(obj, "authentication", "NONE"));
        if (!authentication.has_value()) {
            return EagServer::ErrorResponse(req, status::bad_request, R"(authentication must be "NONE" or "EUCLID")");
        }
        route.authentication = *authentication;
        if (const auto *active = obj.if_contains("active"); active && active->is_bool()) route.active = active->as_bool();

        // A route that was not stored must not come back looking as though it was - the caller
        // would go away believing the path is published, and only find out when nothing serves it.
        const auto saved = repository->upsertRoute(route);
        if (!saved.has_value()) {
            return EagServer::ErrorResponse(req, status::internal_server_error, "Route could not be stored, routeId: " + routeId);
        }
        log_info << "EAG route created, routeId: " << routeId << ", path: " << path << ", application: " << applicationId;

        return EagServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(*saved)));
    }

    static response<string_body> handleUpdateRoute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "update-route");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EagServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EagServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto routeId = stringField(obj, "routeId");
        if (routeId.empty()) return EagServer::ErrorResponse(req, status::bad_request, "routeId is required");

        const auto repository = Database::RepositoryFactory::instance().eagRepository();
        auto route = repository->findRouteByRouteId(routeId);
        if (!route.has_value()) {
            return EagServer::ErrorResponse(req, status::not_found, "Route not found, routeId: " + routeId);
        }

        // Only what was named: an absent field leaves the route as it is, so one setting can be
        // changed without restating the rest.
        // Read before either is applied, since the collision check needs the pair the route will
        // end up with: changing only the methods can just as easily collide as changing the path.
        if (obj.contains("methods")) {
            std::string unknownMethod;
            const auto methods = readMethods(obj, unknownMethod);
            if (!methods.has_value()) {
                return EagServer::ErrorResponse(req, status::bad_request, "Not an HTTP method: " + unknownMethod);
            }
            route->methods = *methods;
        }
        if (obj.contains("path")) {
            const auto path = stringField(obj, "path");
            if (path.empty() || !path.starts_with("/")) {
                return EagServer::ErrorResponse(req, status::bad_request, "path must start with '/'");
            }
            route->path = path;
        }
        if (const auto taken = pathTaken(route->path, route->methods, route->nameSpace, routeId)) {
            return EagServer::ErrorResponse(req, status::conflict, "Path and method are already routed by routeId: " + *taken);
        }
        if (obj.contains("applicationId")) {
            const auto applicationId = stringField(obj, "applicationId");
            if (!Database::RepositoryFactory::instance().eapRepository()->findApplicationByApplicationId(applicationId).has_value()) {
                return EagServer::ErrorResponse(req, status::not_found, "Application not found, applicationId: " + applicationId);
            }
            // Moving a route to an application means it is no longer a module route, and the
            // other way round - leaving both set would make which one wins depend on the proxy.
            route->applicationId = applicationId;
            route->moduleTarget.clear();
            route->moduleAction.clear();
        }
        if (obj.contains("moduleTarget")) {
            const auto moduleTarget = stringField(obj, "moduleTarget");
            if (!kModuleTargets.contains(moduleTarget)) {
                return EagServer::ErrorResponse(req, status::bad_request, "Not a euclid module: " + moduleTarget);
            }
            route->moduleTarget = moduleTarget;
            route->applicationId.clear();
        }
        if (obj.contains("moduleAction")) route->moduleAction = stringField(obj, "moduleAction");

        if (!route->moduleTarget.empty() && route->moduleAction.empty()) {
            return EagServer::ErrorResponse(req, status::bad_request, "moduleAction is required when a module is named");
        }
        if (obj.contains("region")) route->region = stringField(obj, "region");
        if (obj.contains("namespace")) route->nameSpace = stringField(obj, "namespace");
        if (obj.contains("authentication")) {
            // Refused rather than defaulted: somebody asking for EUCLID and silently getting NONE
            // would be handed a public route they believe is protected.
            const auto authentication = RouteAuthenticationFromString(stringField(obj, "authentication"));
            if (!authentication.has_value()) {
                return EagServer::ErrorResponse(req, status::bad_request, R"(authentication must be "NONE" or "EUCLID")");
            }
            route->authentication = *authentication;
        }
        if (const auto *active = obj.if_contains("active"); active && active->is_bool()) route->active = active->as_bool();

        const auto saved = repository->upsertRoute(*route);
        if (!saved.has_value()) {
            return EagServer::ErrorResponse(req, status::internal_server_error, "Route could not be stored, routeId: " + routeId);
        }
        log_info << "EAG route updated, routeId: " << routeId;

        return EagServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(*saved)));
    }

    static response<string_body> handleListRoutes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-routes");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EagServer::ParseJsonBody(req, jv)) return *err;

        std::string prefix;
        if (jv.is_object()) prefix = stringField(jv.as_object(), "prefix");

        boost::json::array routes;
        for (const auto &route: Database::RepositoryFactory::instance().eagRepository()->listRoutes(prefix)) {
            routes.push_back(toJson(route));
        }

        return EagServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"routes", routes}}));
    }

    static response<string_body> handleGetRoute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-route");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EagServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EagServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto routeId = stringField(jv.as_object(), "routeId");
        if (routeId.empty()) return EagServer::ErrorResponse(req, status::bad_request, "routeId is required");

        const auto route = Database::RepositoryFactory::instance().eagRepository()->findRouteByRouteId(routeId);
        if (!route.has_value()) {
            return EagServer::ErrorResponse(req, status::not_found, "Route not found, routeId: " + routeId);
        }

        return EagServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(*route)));
    }

    static response<string_body> handleDeleteRoute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-route");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EagServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EagServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto routeId = stringField(jv.as_object(), "routeId");
        if (routeId.empty()) return EagServer::ErrorResponse(req, status::bad_request, "routeId is required");

        Database::RepositoryFactory::instance().eagRepository()->deleteRoute(routeId);
        log_info << "EAG route deleted, routeId: " << routeId;

        return EagServer::JsonResponse(req, status::ok);
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EagServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EAG action=" << action;

        if (action == "create-route") return handleCreateRoute(req);
        if (action == "update-route") return handleUpdateRoute(req);
        if (action == "list-routes") return handleListRoutes(req);
        if (action == "get-route") return handleGetRoute(req);
        if (action == "delete-route") return handleDeleteRoute(req);
        if (action == "get-metrics") return EagServer::MetricsResponse(req);

        return EagServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    EagServer::EagServer(std::string socketPath, const int threads) : HttpActionServer("EAG", std::move(socketPath), threads) {

        const auto &configuration = Core::Configuration::instance();
        // One port per namespace, or one unscoped port. Keyed by namespace rather than given as a
        // list, so the namespace is the name of the thing rather than a field inside it, and so
        // the same reader euclid.modules already uses can read it:
        //
        //   "listeners": { "development": { "port": 8080 }, "integration": { "port": 8081 } }
        //
        // A production installation is usually its own euclid with one namespace and wants none of
        // this, which is why the bare "port" still works and means a listener that serves every
        // route whatever namespace it names.
        std::vector<ProxyServer::Listener> listeners;
        if (constexpr auto listenerPath = "euclid.modules.eag.listeners"; configuration.has(listenerPath)) {
            try {
                for (const auto &[nameSpace, properties]: configuration.getObjects(listenerPath)) {
                    const auto it = properties.find("port");
                    if (it == properties.end()) {
                        log_error << "API gateway listener has no port and is ignored, namespace: " << nameSpace;
                        continue;
                    }
                    listeners.emplace_back(static_cast<unsigned short>(std::get<long>(it->second)), nameSpace);
                }
            } catch (const std::exception &e) {
                log_error << "Could not read the API gateway listeners, error: " << e.what();
            }
        }
        if (listeners.empty()) {
            listeners.emplace_back(static_cast<unsigned short>(configuration.getOr<long>("euclid.modules.eag.port", 8080)), std::string());
        }
        const auto proxyThreads = static_cast<int>(configuration.getOr<long>("euclid.modules.eag.proxy-threads", 8));
        const auto refreshSeconds = configuration.getOr<long>("euclid.modules.eag.refresh-seconds", 5);

        // How long a verified Basic credential stays verified. Zero hashes the password on every
        // request, which is correct and costs tens of milliseconds of CPU each time.
        const auto basicAuthCacheSeconds = configuration.getOr<long>("euclid.modules.eag.basic-auth-cache-seconds", 60);

        // Where a route naming a euclid module is sent. The same listener the CLI and every SDK
        // already talk to, so nothing new has to be exposed for this - the API gateway simply
        // becomes a second way in to it, on a path of the operator's choosing.
        const auto euclidGatewayPort = static_cast<unsigned short>(configuration.getOr<long>("euclid.gateway.http.port", 5566));

        // The gateway speaks TLS by default, so a module route has to as well - plain HTTP to it
        // is answered with a dropped connection and nothing that says why. Its own certificate is
        // the trust anchor, since a fresh installation's is self-signed.
        const auto euclidGatewayTls = configuration.getOr<bool>("euclid.gateway.tls.enabled", false);
        const auto euclidGatewayCert = configuration.getOr<std::string>("euclid.gateway.tls.cert-file", "");

        try {
            _proxy = std::make_unique<ProxyServer>(listeners, proxyThreads, refreshSeconds, basicAuthCacheSeconds,
                                                   euclidGatewayPort, euclidGatewayTls, euclidGatewayCert);
            _proxy->start();
        } catch (const std::exception &e) {
            // The module still runs: its socket answers, the route table can be managed, and the
            // reason the port is not there is in the log. Throwing would leave the manager
            // restarting a process that says nothing about why it keeps dying.
            std::string ports;
            for (const auto &listener: listeners) {
                if (!ports.empty()) ports += ", ";
                ports += std::to_string(listener.port);
            }
            log_error << "API gateway could not listen on " << ports << ", error: " << e.what();
        }
    }

    EagServer::~EagServer() {
        if (_proxy) _proxy->stop();
    }

    response<string_body> EagServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EAG
