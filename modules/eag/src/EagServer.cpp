//
// Created by vogje01 on 9/5/26.
//

// C++ includes
#include <optional>
#include <string>

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

        // Two routes on the same path would be a tie broken by whichever the sort happened to put
        // first - the caller would see one application today and possibly the other tomorrow. Said
        // here, where somebody is configuring it, rather than left to be discovered in traffic.
        std::optional<std::string> pathTaken(const std::string &path, const std::string &routeId) {
            for (const auto &route: Database::RepositoryFactory::instance().eagRepository()->listRoutes(path)) {
                if (route.path == path && route.routeId != routeId) return route.routeId;
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
        if (routeId.empty()) return EagServer::ErrorResponse(req, status::bad_request, "routeId is required");
        if (applicationId.empty()) return EagServer::ErrorResponse(req, status::bad_request, "applicationId is required");

        // A path that does not start with "/" would never match anything, since what is compared
        // against it is a request target - refused here rather than becoming a route that is
        // configured, listed, and silently dead.
        if (path.empty() || !path.starts_with("/")) {
            return EagServer::ErrorResponse(req, status::bad_request, "path is required and must start with '/'");
        }

        const auto repository = Database::RepositoryFactory::instance().eagRepository();
        if (repository->routeExists(routeId)) {
            return EagServer::ErrorResponse(req, status::conflict, "Route exists already, routeId: " + routeId);
        }
        if (const auto taken = pathTaken(path, routeId)) {
            return EagServer::ErrorResponse(req, status::conflict, "Path is already routed by routeId: " + *taken);
        }

        // The application has to exist. A route to nothing answers 503 for every request, which
        // looks like an application that is down rather than one that was never deployed.
        if (!Database::RepositoryFactory::instance().eapRepository()->findApplicationByApplicationId(applicationId).has_value()) {
            return EagServer::ErrorResponse(req, status::not_found, "Application not found, applicationId: " + applicationId);
        }

        Route route;
        route.routeId = routeId;
        route.path = path;
        route.applicationId = applicationId;
        route.accountId = auth.user->accountId;
        route.region = auth.user->region;
        route.nameSpace = std::string(req["x-euclid-namespace"]);
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
        if (obj.contains("path")) {
            const auto path = stringField(obj, "path");
            if (path.empty() || !path.starts_with("/")) {
                return EagServer::ErrorResponse(req, status::bad_request, "path must start with '/'");
            }
            if (const auto taken = pathTaken(path, routeId)) {
                return EagServer::ErrorResponse(req, status::conflict, "Path is already routed by routeId: " + *taken);
            }
            route->path = path;
        }
        if (obj.contains("applicationId")) {
            const auto applicationId = stringField(obj, "applicationId");
            if (!Database::RepositoryFactory::instance().eapRepository()->findApplicationByApplicationId(applicationId).has_value()) {
                return EagServer::ErrorResponse(req, status::not_found, "Application not found, applicationId: " + applicationId);
            }
            route->applicationId = applicationId;
        }
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
        const auto port = static_cast<unsigned short>(configuration.getOr<long>("euclid.modules.eag.port", 8080));
        const auto proxyThreads = static_cast<int>(configuration.getOr<long>("euclid.modules.eag.proxy-threads", 8));
        const auto refreshSeconds = configuration.getOr<long>("euclid.modules.eag.refresh-seconds", 5);

        try {
            _proxy = std::make_unique<ProxyServer>(port, proxyThreads, refreshSeconds);
            _proxy->start();
        } catch (const std::exception &e) {
            // The module still runs: its socket answers, the route table can be managed, and the
            // reason the port is not there is in the log. Throwing would leave the manager
            // restarting a process that says nothing about why it keeps dying.
            log_error << "API gateway could not listen on port " << port << ", error: " << e.what();
        }
    }

    EagServer::~EagServer() {
        if (_proxy) _proxy->stop();
    }

    response<string_body> EagServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EAG
