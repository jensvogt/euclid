// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <EapServer.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/esm/Object.h>

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
    using Database::Entity::EAP::RedeployRefusal;
    using Database::Entity::EAP::VersionFromArtifactName;

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

        // What an artifact lookup found: the object once ESM has finished with it, or the fact
        // that it is still being processed - two different answers that used to be indistinguish-
        // able, and the second one told a lie.
        struct ArtifactLookup {
            std::optional<Database::Entity::ESM::Object> object;
            bool pending{false};
        };

        // Reads an artifact out of a bucket, waiting out the window in which ESM has accepted an
        // upload but has not finished with it.
        //
        // A multipart "complete-upload" marks the object UPLOADED and returns immediately, leaving
        // the *previous* build's md5Sum and internal file on the row until a background pass
        // assembles the parts, hashes them and advances it to COMPLETED (modules/esm/src/
        // EsmServer.cpp:1189-1306). A redeploy that arrives inside that window therefore compared
        // the new build against its own predecessor's checksum, concluded that nothing would
        // change, and refused - which is exactly why a first redeploy failed and an identical
        // second one, a moment later, went through.
        //
        // Waiting rather than skipping the comparison: the assembled file is also what euclid-mgr
        // copies when it restarts the pool, so going ahead before COMPLETED could put the previous
        // build on disk under the new version's name. An artifact that is not there at all is not
        // waited for - create-upload seeds the row before the first part arrives, so a missing row
        // means a key nobody is uploading to, and a typo should be answered now rather than in
        // half a minute.
        //
        // The bound stays well inside the clients' own request timeouts (the RUI aborts at 15 s),
        // so a wait that does expire produces this module's explanation rather than their generic
        // network error. A JAR settles in well under a second; only something very large runs out,
        // and for that "try again shortly" is the honest answer.
        ArtifactLookup findSettledArtifact(const std::string &bucketErn, const std::string &key) {
            using Database::Entity::ESM::ObjectStatus;
            constexpr auto kSettleWait = std::chrono::seconds(10);
            constexpr auto kPollInterval = std::chrono::milliseconds(100);

            const auto esmRepository = Database::RepositoryFactory::instance().esmRepository();
            const auto deadline = std::chrono::steady_clock::now() + kSettleWait;

            ArtifactLookup result;
            for (;;) {
                auto object = esmRepository->findObjectByBucketAndKey(bucketErn, key);
                if (!object.has_value()) return result;

                // Only the three states of an upload in flight are waited for. Anything else is
                // taken as it stands, including the UNKNOWN of a row written before objects
                // carried a status - refusing those would strand every application deployed from
                // one of them.
                const auto status = object->status;
                if (status != ObjectStatus::CREATED && status != ObjectStatus::UPLOADING && status != ObjectStatus::UPLOADED) {
                    result.object = std::move(object);
                    result.pending = false;
                    return result;
                }

                result.pending = true;
                if (std::chrono::steady_clock::now() >= deadline) return result;
                std::this_thread::sleep_for(kPollInterval);
            }
        }

        // The message for an artifact whose upload has not settled, kept in one place so all three
        // handlers say the same thing.
        std::string artifactPendingMessage(const std::string &artifactKey) {
            return "The artifact '" + artifactKey + "' is still being assembled by ESM. Its checksum is not known yet, "
                   "so deploying it now could put the previous build on disk. Try again in a moment.";
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

        // The identity an application runs as, when the caller did not name one of their own.
        //
        // A technical principal rather than a person: no password, no login (see
        // Entity::EAM::User::loginEnabled), one access key. That key is the whole of what the
        // application can do - it signs the calls it makes, and euclid attributes them to this
        // principal instead of to whoever happened to deploy it. Nobody has to hand an application
        // a human's credentials, and nothing an application leaks is a person's.
        std::string technicalUserId(const std::string &applicationId) {
            return "app-" + applicationId;
        }

        // Turns the bucket and queue names a caller deployed the application with into the ERNs the
        // storage and queueing modules check against. Names are what an operator has in hand;
        // ERNs are what a grant has to be written in, since a name is only unique within an
        // account and namespace.
        //
        // Returns std::nullopt on the first name that does not resolve, so a typo is reported at
        // deployment rather than becoming an application that is quietly denied at run time.
        std::optional<std::vector<std::string> > resolveResources(const std::vector<std::string> &buckets, const std::vector<std::string> &queues, std::string &unresolved) {

            std::vector<std::string> resources;

            for (const auto &name: buckets) {
                const auto bucket = Database::RepositoryFactory::instance().esmRepository()->findBucketByName(name);
                if (!bucket.has_value()) {
                    unresolved = "bucket '" + name + "'";
                    return std::nullopt;
                }
                resources.push_back(bucket->ern);
            }

            for (const auto &name: queues) {
                const auto queue = Database::RepositoryFactory::instance().eqsRepository()->findQueueByName(name);
                if (!queue.has_value()) {
                    unresolved = "queue '" + name + "'";
                    return std::nullopt;
                }
                resources.push_back(queue->ern);
            }
            return resources;
        }

        Database::Entity::EAM::User createTechnicalUser(const std::string &applicationId, const std::string &accountId,
                                                        const std::string &region, const std::string &nameSpace,
                                                        const std::vector<std::string> &resources) {

            const auto now = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());

            Database::Entity::EAM::AccessKey key;
            key.accessKeyId = Core::CryptoUtils::GenerateAccessKeyId();
            key.secretAccessKey = Core::CryptoUtils::GenerateSecretAccessKey();
            key.active = true;
            key.created = now;

            // Without a grant the principal can authenticate and do nothing: every module runs the
            // caller's account through Core::HttpActionServer::GrantLookup, and a user with no
            // grant for the account it names is refused. So it is granted its own account here -
            // the same account the application belongs to, and no other - plus the namespace the
            // application was created in, since a request that names one is checked against the
            // grant's namespace list rather than the account alone.
            Database::Entity::EAM::AccountGrant grant;
            grant.accountId = accountId;
            grant.isAdmin = false;
            grant.granted = now;
            if (!nameSpace.empty()) grant.namespaces.push_back(nameSpace);

            Database::Entity::EAM::User user;
            user.userId = technicalUserId(applicationId);
            user.accountId = accountId;
            user.region = region;
            user.ern = Core::createEamUserErn(accountId, user.userId);
            // Not a hash of anything: PasswordUtils::Verify() cannot match an empty stored
            // password, so there is no password to guess even before loginEnabled is consulted.
            user.password = "";
            // A principal has no mailbox, but it cannot have an empty address either: eam_user
            // carries a unique index on email, and while that index is sparse it only skips
            // documents with no email field at all - "" is a value, and the second principal to
            // use it collides with the first. The domain is deliberately under .invalid, which
            // RFC 2606 reserves so that it can never resolve to anything real.
            user.email = user.userId + "@euclid.invalid";
            user.loginEnabled = false;
            user.accessKeys.push_back(key);
            user.accountGrants.push_back(grant);
            // Named resources narrow the principal to exactly them. Naming none leaves it able to
            // reach everything in its account, which is what an application that was deployed
            // without saying what it needs has always been able to do.
            user.resourceGrants = resources;

            const auto stored = Database::RepositoryFactory::instance().eamRepository()->upsertUser(user);
            log_info << "EAP created technical user, userId: " << user.userId << ", accessKeyId: " << key.accessKeyId;
            return stored;
        }

        // Whether this principal is one EAP made for this application, and may therefore be
        // removed with it. A user the caller named themselves is theirs, and is left alone.
        bool isOwnedTechnicalUser(const std::string &applicationId, const std::string &userId) {
            if (userId != technicalUserId(applicationId)) return false;
            const auto user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(userId);
            return user.has_value() && !user->loginEnabled;
        }

        boost::json::object toJson(const Application &application) {

            boost::json::array arguments;
            for (const auto &argument: application.arguments) arguments.push_back(boost::json::string(argument));

            boost::json::object environment;
            for (const auto &[name, value]: application.environment) environment[name] = value;

            boost::json::array resources;
            for (const auto &resource: application.resources) resources.push_back(boost::json::string(resource));

            const auto running = runningInstances(application.applicationId);

            return boost::json::object{
                    {"applicationId", application.applicationId},
                    {"ern", application.ern},
                    {"accountId", application.accountId},
                    {"region", application.region},
                    {"runtime", RuntimeToString(application.runtime)},
                    {"bucketErn", application.bucketErn},
                    {"artifactKey", application.artifactKey},
                    {"version", application.version},
                    {"md5Sum", application.md5Sum},
                    {"command", application.command},
                    {"arguments", arguments},
                    {"environment", environment},
                    {"resources", resources},
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
        const auto lookup = findSettledArtifact(bucket->ern, artifactKey);
        if (!lookup.object.has_value()) {
            if (lookup.pending) {
                return EapServer::ErrorResponse(req, status::conflict, artifactPendingMessage(artifactKey));
            }
            return EapServer::ErrorResponse(req, status::not_found, "Artifact not found in bucket '" + bucketName + "': " + artifactKey);
        }
        const auto &artifact = lookup.object;

        // Which build this is has to be answerable from the definition alone, so it is settled
        // here: taken from the artifact's name when it carries one, and asked for when it does
        // not. Nothing later can supply it - a redeploy is refused unless the version changes, and
        // there is nothing to compare against if the first one was never recorded.
        auto version = stringField(obj, "version");
        if (version.empty()) version = VersionFromArtifactName(artifactKey);
        if (version.empty()) {
            return EapServer::ErrorResponse(req, status::bad_request,
                                            "version is required: it could not be read from the artifact name '" + artifactKey + "' (expected something like 1.4.0)");
        }

        // The application runs as some identity and signs its own calls back into euclid with that
        // identity's access key. Left unnamed, it gets one of its own: a technical principal
        // created here and removed with the application, which is what keeps an application from
        // having to borrow a person's credentials.
        //
        // A caller can still name an existing user - for an application that genuinely should act
        // as somebody - in which case that user has to exist and have a key already. Refused here
        // rather than at start-up for the same reason as the artifact: an application that comes
        // up and then cannot talk to anything is a worse failure than a rejected deployment.
        // What this application is allowed to touch, resolved from names to ERNs now so a typo is
        // a rejected deployment rather than an application that runs and is denied everything.
        std::string unresolved;
        const auto resources = resolveResources(stringArray(obj, "buckets"), stringArray(obj, "queues"), unresolved);
        if (!resources.has_value()) {
            return EapServer::ErrorResponse(req, status::not_found, "Not found: " + unresolved);
        }

        auto userId = stringField(obj, "user");
        if (userId.empty()) {
            userId = createTechnicalUser(applicationId, auth.user->accountId, auth.user->region,
                                         std::string(req["x-euclid-namespace"]), *resources)
                             .userId;
        } else {
            const auto user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(userId);
            if (!user.has_value()) {
                return EapServer::ErrorResponse(req, status::not_found, "User not found: " + userId);
            }
            if (user->accessKeys.empty()) {
                return EapServer::ErrorResponse(req, status::bad_request,
                                                "User '" + userId + "' has no access key - create one with 'euclid-cli eam create-access-key' so the application can authenticate");
            }
        }

        Application application;
        application.applicationId = applicationId;
        application.accountId = auth.user->accountId;
        application.region = auth.user->region;
        // The namespace the application will work in, which is the one it is being created in -
        // the same one its technical user is granted just above. Written here so the manager can
        // hand it to the application in its credentials, which is what lets the application name
        // a queue or topic instead of spelling out a full ERN.
        application.nameSpace = std::string(req["x-euclid-namespace"]);
        application.ern = Core::createEapApplicationErn(application.accountId, applicationId);
        application.runtime = runtime;
        application.bucketErn = bucket->ern;
        application.artifactKey = artifactKey;
        application.version = version;
        // The checksum is ESM's, not one computed here: it is the same hash the manager compares
        // the copy on the host against, and the same one a redeploy has to differ from.
        application.md5Sum = artifact->md5Sum;
        application.command = stringField(obj, "command");
        application.arguments = stringArray(obj, "arguments");
        application.environment = stringMap(obj, "environment");
        application.resources = *resources;
        application.userId = userId;
        application.minInstances = std::max(1L, longField(obj, "minInstances", 1));
        application.maxInstances = std::max(application.minInstances, longField(obj, "maxInstances", 1));
        application.readyTimeoutMs = std::max(1000L, longField(obj, "readyTimeoutMs", 30000));
        application.desiredState = ApplicationState::STOPPED;

        const auto stored = repo->upsertApplication(application);
        log_info << "EAP created application, applicationId: " << stored.applicationId << ", runtime: " << RuntimeToString(stored.runtime)
                << ", artifact: " << stored.artifactKey << ", version: " << stored.version << ", user: " << stored.userId;

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
            const auto lookup = findSettledArtifact(application->bucketErn, artifactKey);
            if (!lookup.object.has_value()) {
                if (lookup.pending) {
                    return EapServer::ErrorResponse(req, status::conflict, artifactPendingMessage(artifactKey));
                }
                return EapServer::ErrorResponse(req, status::not_found, "Artifact not found: " + artifactKey);
            }
            const auto &artifact = lookup.object;
            application->artifactKey = artifactKey;

            // Deliberately without the checks redeploy-application makes: this is the way to
            // deploy a build that a redeploy would refuse - the same version rebuilt, a rollback
            // to a version already seen - and the recorded checksum has to follow the artifact
            // either way, or the definition would claim a build it no longer points at.
            application->md5Sum = artifact->md5Sum;
        }
        // The namespace an application works in, changeable like any other setting - and the only
        // way an application created before applications carried one can acquire it without being
        // deleted and made again. Sending it empty moves the application back to the account root,
        // which is a legitimate thing to ask for, so absent and empty mean different things here.
        if (obj.contains("namespace")) application->nameSpace = stringField(obj, "namespace");
        if (obj.contains("version")) application->version = stringField(obj, "version");
        if (obj.contains("command")) application->command = stringField(obj, "command");
        if (obj.contains("arguments")) application->arguments = stringArray(obj, "arguments");
        if (obj.contains("environment")) application->environment = stringMap(obj, "environment");
        if (obj.contains("minInstances")) application->minInstances = std::max(1L, longField(obj, "minInstances", application->minInstances));
        if (obj.contains("maxInstances")) application->maxInstances = std::max(application->minInstances, longField(obj, "maxInstances", application->maxInstances));
        if (obj.contains("readyTimeoutMs")) application->readyTimeoutMs = std::max(1000L, longField(obj, "readyTimeoutMs", application->readyTimeoutMs));

        if (obj.contains("buckets") || obj.contains("queues")) {
            std::string unresolved;
            const auto resources = resolveResources(stringArray(obj, "buckets"), stringArray(obj, "queues"), unresolved);
            if (!resources.has_value()) {
                return EapServer::ErrorResponse(req, status::not_found, "Not found: " + unresolved);
            }
            application->resources = *resources;

            // The grants live on the principal, which is what the other modules read - so a
            // definition whose resources changed and whose principal did not would go on being
            // enforced against the old list. Only a principal EAP owns is rewritten; a user the
            // caller named is theirs to grant.
            if (isOwnedTechnicalUser(applicationId, application->userId)) {
                const auto eamRepository = Database::RepositoryFactory::instance().eamRepository();
                if (auto principal = eamRepository->findUserByUserId(application->userId); principal.has_value()) {
                    principal->resourceGrants = *resources;
                    eamRepository->upsertUser(*principal);
                    log_info << "EAP updated technical user resource grants, userId: " << principal->userId
                            << ", resources: " << resources->size();
                }
            }
        }

        const auto stored = repo->upsertApplication(*application);
        log_info << "EAP updated application, applicationId: " << stored.applicationId;

        return EapServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(stored)));
    }

    /**
     * @brief Deploys a new build of an application: an artifact that differs from the one running.
     *
     * Separate from update-application because it is the one change with a rule attached. A
     * deployment is supposed to move an application from one build to another, and there is one
     * way that silently fails to happen: the artifact hashes the same, so the "new build" is the
     * build already deployed and the restart it would cause buys nothing. That is refused.
     *
     * The version is not part of the rule. Redeploying the same version with different bytes is
     * ordinary - a rebuilt snapshot, a fix that keeps the number - and it is the artifact, not its
     * name, that says whether anything is being deployed.
     *
     * update-application remains the way to repoint an application without this check.
     */
    static response<string_body> handleRedeployApplication(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "redeploy-application");

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

        // A redeploy under the same key is the common case - the build changes, its name does not -
        // so the artifact is optional and defaults to the one already deployed.
        const auto artifactKey = stringField(obj, "artifact", application->artifactKey);
        // Settled, not merely present: the checksum below is the whole decision, and an artifact
        // mid-upload still carries the one belonging to the build being replaced.
        const auto lookup = findSettledArtifact(application->bucketErn, artifactKey);
        if (!lookup.object.has_value()) {
            if (lookup.pending) {
                return EapServer::ErrorResponse(req, status::conflict, artifactPendingMessage(artifactKey));
            }
            return EapServer::ErrorResponse(req, status::not_found, "Artifact not found: " + artifactKey);
        }
        const auto &artifact = lookup.object;

        auto version = stringField(obj, "version");
        if (version.empty()) version = VersionFromArtifactName(artifactKey);
        if (version.empty()) {
            return EapServer::ErrorResponse(req, status::bad_request,
                                            "version is required: it could not be read from the artifact name '" + artifactKey + "' (expected something like 1.4.0)");
        }

        if (const auto refused = RedeployRefusal(application->version, application->md5Sum, version, artifact->md5Sum); !refused.empty()) {
            return EapServer::ErrorResponse(req, status::conflict,
                                            "Refusing to redeploy '" + applicationId + "': " + refused
                                                    + ". Use 'update-application' to deploy it anyway.");
        }

        const auto previousVersion = application->version;
        application->artifactKey = artifactKey;
        application->version = version;
        application->md5Sum = artifact->md5Sum;

        // Storing it is what deploys it: upsertApplication stamps `modified`, the manager reads
        // that as a new revision on its next pass, and restarts the pool onto the new artifact.
        const auto stored = repo->upsertApplication(*application);
        log_info << "EAP redeployed application, applicationId: " << stored.applicationId << ", version: "
                << (previousVersion.empty() ? "(none)" : previousVersion) << " -> " << stored.version << ", artifact: " << stored.artifactKey;

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
        const auto application = repo->findApplicationByApplicationId(applicationId);
        repo->deleteApplication(applicationId);
        log_info << "EAP deleted application, applicationId: " << applicationId;

        // The principal goes with the application it was made for - a credential outliving the
        // thing it was issued to is exactly the orphan this arrangement exists to avoid. A user
        // the caller named themselves is left alone.
        if (application.has_value() && isOwnedTechnicalUser(applicationId, application->userId)) {
            Database::RepositoryFactory::instance().eamRepository()->deleteUser(application->userId);
            log_info << "EAP deleted technical user, userId: " << application->userId;
        }

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
        if (action == "redeploy-application") return handleRedeployApplication(req);
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
