// C++ includes
#include <algorithm>
#include <unordered_set>

// Mongo includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/replace.hpp>

// Euclid includes
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/database/Database.h>
#include <EmmServer.h>

namespace Euclid::EMM {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EmmServer::Authenticate(), by user ID, and
        // confirms administrator-group membership - every emm action is administrator-only, since
        // it exposes/mutates every module's live process pool (pids, sockets, restart history)
        // and raw database collections.
        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
            bool forbidden{false};
        };

        constexpr auto kServiceTimer = "emm-service-time";
        constexpr auto kServiceCounter = "emm-service-count";

        // Which of a module's own MongoDB collections "export" dumps. topLevel is what --full-less
        // export includes - the same resources euclid-cli's own list-* actions show (queues, topics,
        // buckets, users, ...). fullOnly is the bulk child data each of those resources owns
        // (EQS/ENS messages, ESM objects) - opt-in via --full, since it can dwarf the container
        // records around it and usually isn't what an operator wants for a quick inspection/backup.
        struct ModuleExportSpec {
            std::vector<std::string> topLevel;
            std::vector<std::string> fullOnly;
        };

        const std::unordered_map<std::string, ModuleExportSpec> &moduleExportSpecs() {
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
        boost::json::array exportCollection(mongocxx::collection collection) {
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
        const std::unordered_map<std::string, std::string> &collectionModules() {
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
        std::pair<long, long> importCollection(mongocxx::collection collection, const boost::json::array &docs) {
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
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EmmServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto user = repo->findUserByUserId(*auth.subject);
        if (!user.has_value() || !Database::IsEamAdmin(*repo, user->userId)) {
            return {.user = std::nullopt, .forbidden = true};
        }
        return {.user = user};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        if (auth.forbidden) {
            return EmmServer::ErrorResponse(req, status::forbidden, "administrator privileges required");
        }
        return EmmServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // ── Action handlers ──────────────────────────────────────────────────────

    static response<string_body> handleListModules(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-modules");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

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
                    {"minInstances", m.minInstances},
                    {"maxInstances", m.maxInstances},
                    {"created", Core::DateTimeUtils::ToISO8601(m.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(m.modified)},
                    {"lastStartTime", m.lastStartTime.time_since_epoch().count() == 0 ? boost::json::value(nullptr) : boost::json::value(Core::DateTimeUtils::ToISO8601(m.lastStartTime))},
                    {"instances", std::move(instances)},
            });
        }
        const auto total = static_cast<long>(modules.size());
        return EmmServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"modules", std::move(modules)},
                                               {"total", total},
                                       }));
    }

    static response<string_body> handleExport(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "export");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv = boost::json::object{};
        if (!req.body().empty()) {
            try {
                jv = boost::json::parse(req.body());
            } catch (const std::exception &) {
                return EmmServer::ErrorResponse(req, status::bad_request, "invalid JSON body");
            }
        }
        const auto all = Core::GetBoolValue(jv, "all");
        const auto requestedModules = Core::GetStringArrayValue(jv, "modules");
        const auto full = Core::GetBoolValue(jv, "full");

        if (all == !requestedModules.empty()) {
            return EmmServer::ErrorResponse(req, status::bad_request, "exactly one of \"all\" or \"modules\" is required");
        }

        const auto &specs = moduleExportSpecs();
        std::vector<std::string> modules;
        if (all) {
            for (const auto &[name, spec]: specs) modules.push_back(name);
            std::ranges::sort(modules);
        } else {
            for (const auto &module: requestedModules) {
                if (!specs.contains(module)) {
                    return EmmServer::ErrorResponse(req, status::bad_request, "unknown module '" + module + "' (expected one of: eam, emm, emo, ens, eqs, esm)");
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
        return EmmServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"modules", std::move(modulesJson)},
                                               {"full", full},
                                               {"exportedAt", Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())},
                                               {"collections", std::move(collections)},
                                       }));
    }

    static response<string_body> handleImport(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "import");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        if (req.body().empty()) {
            return EmmServer::ErrorResponse(req, status::bad_request, "missing JSON body");
        }
        boost::json::value jv;
        try {
            jv = boost::json::parse(req.body());
        } catch (const std::exception &) {
            return EmmServer::ErrorResponse(req, status::bad_request, "invalid JSON body");
        }
        if (!jv.is_object() || !Core::AttributeExists(jv, "collections") || !jv.at("collections").is_object()) {
            return EmmServer::ErrorResponse(req, status::bad_request, "missing \"collections\" object");
        }

        // Optional filter, e.g. re-importing only "esm" out of a multi-module export file; an
        // empty filter (the common case - re-importing everything the file contains) means no
        // restriction.
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

        return EmmServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"imported", std::move(imported)},
                                               {"skipped", std::move(skipped)},
                                       }));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the EMM service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            ListModules,
            Export,
            Import
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "list-modules") return Command::ListModules;
        if (action == "export") return Command::Export;
        if (action == "import") return Command::Import;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EmmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EMM action=" << action;

        switch (commandFromString(action)) {

            case Command::ListModules:
                return handleListModules(req);

            case Command::Export:
                return handleExport(req);

            case Command::Import:
                return handleImport(req);

            case Command::Unknown:
            default:
                log_warning << "Unknown action: " << action;
                return EmmServer::ErrorResponse(req, status::not_found, "unknown emm action '" + action + "'");
        }
    }

    // ── EmmServer ────────────────────────────────────────────────────────────

    EmmServer::EmmServer(std::string socketPath, const int threads) : HttpActionServer("EMM", std::move(socketPath), threads) {}

    response<string_body> EmmServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EMM
