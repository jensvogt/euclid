// C++ includes
#include <map>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <EapServer.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/entity/eam/User.h>

namespace Euclid::EAP {

    namespace {
        constexpr auto kServiceTimer = "eap-service-time";
        constexpr auto kServiceCounter = "eap-service-count";
    }// namespace

    using Database::Entity::EAP::Application;
    using Database::Entity::EAP::ApplicationState;
    using Database::Entity::EAP::ApplicationStateToString;
    using Database::Entity::EAP::Runtime;
    using Database::Entity::EAP::RuntimeFromString;
    using Database::Entity::EAP::RuntimeToString;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        std::string stringField(const boost::json::object &obj, const std::string &key, const std::string &fallback = "") {
            if (const auto *v = obj.if_contains(key); v && v->is_string()) return v->as_string().c_str();
            return fallback;
        }

        long longField(const boost::json::object &obj, const std::string &key, const long fallback = 0) {
            if (const auto *v = obj.if_contains(key); v && v->is_number()) return v->to_number<long>();
            return fallback;
        }

        std::vector<std::string> stringArray(const boost::json::object &obj, const std::string &key) {
            std::vector<std::string> result;
            if (const auto *v = obj.if_contains(key); v && v->is_array()) {
                for (const auto &element: v->as_array()) {
                    if (element.is_string()) result.emplace_back(element.as_string().c_str());
                }
            }
            return result;
        }

        std::map<std::string, std::string> stringMap(const boost::json::object &obj, const std::string &key) {
            std::map<std::string, std::string> result;
            if (const auto *v = obj.if_contains(key); v && v->is_object()) {
                for (const auto &[name, value]: v->as_object()) {
                    if (value.is_string()) result[std::string(name)] = value.as_string().c_str();
                }
            }
            return result;
        }

        // The manager publishes each module's live instances, so an application's *observed* state
        // is read from there rather than stored - the definition only ever carries what it should
        // be. Reported as a count as well as a state, because an application is a pool: knowing
        // that three of it are up is the thing an operator actually wants.
        long runningInstances(const std::string &applicationId) {
            long running = 0;
            for (const auto &module: Database::RepositoryFactory::instance().emmRepository()->findAll()) {
                if (module.name != applicationId) continue;
                for (const auto &instance: module.instances) {
                    if (instance.state == Database::Entity::ModuleState::RUNNING) ++running;
                }
            }
            return running;
        }

        boost::json::object toJson(const Application &application) {

            boost::json::array arguments;
            for (const auto &argument: application.arguments) arguments.push_back(boost::json::string(argument));

            boost::json::object environment;
            for (const auto &[name, value]: application.environment) environment[name] = value;

            const auto running = runningInstances(application.applicationId);

            return boost::json::object{
                    {"applicationId", application.applicationId},
                    {"ern", application.ern},
                    {"accountId", application.accountId},
                    {"region", application.region},
                    {"runtime", RuntimeToString(application.runtime)},
                    {"bucketErn", application.bucketErn},
                    {"artifactKey", application.artifactKey},
                    {"command", application.command},
                    {"arguments", arguments},
                    {"environment", environment},
                    {"userId", application.userId},
                    {"minInstances", application.minInstances},
                    {"maxInstances", application.maxInstances},
                    {"readyTimeoutMs", application.readyTimeoutMs},
                    {"desiredState", ApplicationStateToString(application.desiredState)},
                    {"state", running > 0 ? "RUNNING" : "STOPPED"},
                    {"instances", running},
                    {"created", Core::DateTimeUtils::ToISO8601(application.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(application.modified)}};
        }

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EapServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EapServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Every action here decides which code euclid executes, and under whose identity it does so,
    // which is about as privileged as this system gets - so all of them are administrator-only.
    static std::optional<response<string_body> > requireAdmin(const request<string_body> &req, AuthResult &auth) {
        auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), auth.user->userId)) {
            return EapServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }
        return std::nullopt;
    }

    // ── Action handlers ──────────────────────────────────────────────────────

    static response<string_body> handleCreateApplication(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-application");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EapServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EapServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto applicationId = stringField(obj, "applicationId");
        if (applicationId.empty()) return EapServer::ErrorResponse(req, status::bad_request, "applicationId is required");

        const auto repo = Database::RepositoryFactory::instance().eapRepository();
        if (repo->applicationExists(applicationId)) {
            return EapServer::ErrorResponse(req, status::conflict, "Application already exists: " + applicationId);
        }

        const auto runtime = RuntimeFromString(stringField(obj, "runtime"));
        if (runtime == Runtime::UNKNOWN) {
            return EapServer::ErrorResponse(req, status::bad_request, "runtime must be one of JAVA, PYTHON, NODEJS, BINARY");
        }

        // The bucket and the artifact are resolved now rather than at start-up, so a typo is
        // reported to whoever deploys the application instead of surfacing later as a pool that
        // never comes up.
        const auto bucketName = stringField(obj, "bucket");
        if (bucketName.empty()) return EapServer::ErrorResponse(req, status::bad_request, "bucket is required");

        const auto esmRepository = Database::RepositoryFactory::instance().esmRepository();
        const auto bucket = esmRepository->findBucketByName(bucketName);
        if (!bucket.has_value()) {
            return EapServer::ErrorResponse(req, status::not_found, "Bucket not found: " + bucketName);
        }

        const auto artifactKey = stringField(obj, "artifact");
        if (artifactKey.empty()) return EapServer::ErrorResponse(req, status::bad_request, "artifact is required");
        if (!esmRepository->findObjectByBucketAndKey(bucket->ern, artifactKey).has_value()) {
            return EapServer::ErrorResponse(req, status::not_found, "Artifact not found in bucket '" + bucketName + "': " + artifactKey);
        }

        // The application runs as this user and signs its own calls back into euclid with that
        // user's access key, so both have to exist before it can do anything useful. Refused here
        // rather than at start-up for the same reason as the artifact: an application that comes
        // up and then cannot talk to anything is a worse failure than a rejected deployment.
        const auto userId = stringField(obj, "user");
        if (userId.empty()) return EapServer::ErrorResponse(req, status::bad_request, "user is required");

        const auto user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(userId);
        if (!user.has_value()) {
            return EapServer::ErrorResponse(req, status::not_found, "User not found: " + userId);
        }
        if (user->accessKeys.empty()) {
            return EapServer::ErrorResponse(req, status::bad_request,
                                            "User '" + userId + "' has no access key - create one with 'euclid-cli eam create-access-key' so the application can authenticate");
        }

        Application application;
        application.applicationId = applicationId;
        application.accountId = auth.user->accountId;
        application.region = auth.user->region;
        application.ern = Core::createEapApplicationErn(application.accountId, applicationId);
        application.runtime = runtime;
        application.bucketErn = bucket->ern;
        application.artifactKey = artifactKey;
        application.command = stringField(obj, "command");
        application.arguments = stringArray(obj, "arguments");
        application.environment = stringMap(obj, "environment");
        application.userId = userId;
        application.minInstances = std::max(1L, longField(obj, "minInstances", 1));
        application.maxInstances = std::max(application.minInstances, longField(obj, "maxInstances", 1));
        application.readyTimeoutMs = std::max(1000L, longField(obj, "readyTimeoutMs", 30000));
        application.desiredState = ApplicationState::STOPPED;

        const auto stored = repo->upsertApplication(application);
        log_info << "EAP created application, applicationId: " << stored.applicationId << ", runtime: " << RuntimeToString(stored.runtime)
                << ", artifact: " << stored.artifactKey << ", user: " << stored.userId;

        return EapServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(stored)));
    }

    static response<string_body> handleUpdateApplication(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "update-application");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EapServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EapServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto applicationId = stringField(obj, "applicationId");
        const auto repo = Database::RepositoryFactory::instance().eapRepository();
        auto application = repo->findApplicationByApplicationId(applicationId);
        if (!application.has_value()) {
            return EapServer::ErrorResponse(req, status::not_found, "Application not found: " + applicationId);
        }

        // Only what the caller actually sent is touched: an update that named a single field
        // would otherwise reset everything else to its default.
        if (obj.contains("runtime")) {
            const auto runtime = RuntimeFromString(stringField(obj, "runtime"));
            if (runtime == Runtime::UNKNOWN) {
                return EapServer::ErrorResponse(req, status::bad_request, "runtime must be one of JAVA, PYTHON, NODEJS, BINARY");
            }
            application->runtime = runtime;
        }
        if (obj.contains("artifact")) {
            const auto artifactKey = stringField(obj, "artifact");
            if (!Database::RepositoryFactory::instance().esmRepository()->findObjectByBucketAndKey(application->bucketErn, artifactKey).has_value()) {
                return EapServer::ErrorResponse(req, status::not_found, "Artifact not found: " + artifactKey);
            }
            application->artifactKey = artifactKey;
        }
        if (obj.contains("command")) application->command = stringField(obj, "command");
        if (obj.contains("arguments")) application->arguments = stringArray(obj, "arguments");
        if (obj.contains("environment")) application->environment = stringMap(obj, "environment");
        if (obj.contains("minInstances")) application->minInstances = std::max(1L, longField(obj, "minInstances", application->minInstances));
        if (obj.contains("maxInstances")) application->maxInstances = std::max(application->minInstances, longField(obj, "maxInstances", application->maxInstances));
        if (obj.contains("readyTimeoutMs")) application->readyTimeoutMs = std::max(1000L, longField(obj, "readyTimeoutMs", application->readyTimeoutMs));

        const auto stored = repo->upsertApplication(*application);
        log_info << "EAP updated application, applicationId: " << stored.applicationId;

        return EapServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(stored)));
    }

    static response<string_body> handleListApplications(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-applications");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EapServer::ParseJsonBody(req, jv)) return *err;

        std::string prefix;
        if (jv.is_object()) prefix = stringField(jv.as_object(), "prefix");

        boost::json::array applications;
        for (const auto &application: Database::RepositoryFactory::instance().eapRepository()->listApplications(prefix)) {
            applications.push_back(toJson(application));
        }

        return EapServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"applications", applications}}));
    }

    static response<string_body> handleGetApplication(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-application");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EapServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EapServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto applicationId = stringField(jv.as_object(), "applicationId");
        const auto application = Database::RepositoryFactory::instance().eapRepository()->findApplicationByApplicationId(applicationId);
        if (!application.has_value()) {
            return EapServer::ErrorResponse(req, status::not_found, "Application not found: " + applicationId);
        }

        return EapServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(*application)));
    }

    static response<string_body> handleDeleteApplication(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-application");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EapServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EapServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto applicationId = stringField(jv.as_object(), "applicationId");
        const auto repo = Database::RepositoryFactory::instance().eapRepository();
        if (!repo->applicationExists(applicationId)) {
            return EapServer::ErrorResponse(req, status::not_found, "Application not found: " + applicationId);
        }

        // Deleting the definition is what stops the processes: the reconciler runs whatever is
        // defined and RUNNING, so a definition that no longer exists is torn down on the next tick.
        repo->deleteApplication(applicationId);
        log_info << "EAP deleted application, applicationId: " << applicationId;

        return EapServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"applicationId", applicationId}, {"deleted", true}}));
    }

    // start-application and stop-application only record intent; euclid-mgr's reconciler is what
    // acts on it.
    static response<string_body> handleSetState(const request<string_body> &req, const ApplicationState desired) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method",
                                                  desired == ApplicationState::RUNNING ? "start-application" : "stop-application");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EapServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EapServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto applicationId = stringField(jv.as_object(), "applicationId");
        const auto repo = Database::RepositoryFactory::instance().eapRepository();
        auto application = repo->findApplicationByApplicationId(applicationId);
        if (!application.has_value()) {
            return EapServer::ErrorResponse(req, status::not_found, "Application not found: " + applicationId);
        }

        application->desiredState = desired;
        const auto stored = repo->upsertApplication(*application);
        log_info << "EAP set application state, applicationId: " << applicationId << ", desiredState: " << ApplicationStateToString(desired);

        return EapServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(stored)));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EapServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EAP action=" << action;

        if (action == "create-application") return handleCreateApplication(req);
        if (action == "update-application") return handleUpdateApplication(req);
        if (action == "list-applications") return handleListApplications(req);
        if (action == "get-application") return handleGetApplication(req);
        if (action == "delete-application") return handleDeleteApplication(req);
        if (action == "start-application") return handleSetState(req, ApplicationState::RUNNING);
        if (action == "stop-application") return handleSetState(req, ApplicationState::STOPPED);
        if (action == "get-metrics") return EapServer::MetricsResponse(req);

        return EapServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EapServer ────────────────────────────────────────────────────────────

    EapServer::EapServer(std::string socketPath, const int threads) : HttpActionServer("EAP", std::move(socketPath), threads) {}

    response<string_body> EapServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EAP
