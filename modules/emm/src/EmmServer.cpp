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
#include <euclid/core/CryptoUtils.h>
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
                    {"eap", {{"eap_application"}, {}}},
                    {"ets", {{"ets_server"}, {}}},
                    // Note what an ekm export carries: Database::Entity::EKM::Key stores its
                    // material base64-encoded but not encrypted, on the understanding that it
                    // never leaves the server. Here it does - a backup of a key store that omits
                    // the keys restores nothing - so the file is as sensitive as the database it
                    // came from and wants the same handling.
                    {"ekm", {{"ekm_key"}, {}}},
            };
            return specs;
        }


        // ── Passphrase-protected archives ────────────────────────────────────
        //
        // An export is a file of everything the modules hold - access-key secrets among it, and
        // key material if ekm was asked for - which then lives wherever the operator put it. So it
        // can be sealed: the passphrase is turned into an AES key here and the payload comes back
        // encrypted, leaving nothing on disk that is worth anything without the passphrase.
        //
        // Note what this is not. It protects the file, not the action: anyone who may call
        // "export" may call it again without a passphrase. And the passphrase reaches this process
        // to be used, so it wants the same care as a password on the way in - which is why nothing
        // here logs a request body.

        constexpr int kKdfIterations = 600000;
        constexpr std::size_t kSaltLength = 16;
        constexpr std::size_t kKeyLength = 32;
        constexpr auto kKdfName = "pbkdf2-sha256";
        constexpr auto kCipherName = "aes-256-gcm";

        // Base64 of the salt this archive's key was derived with, and the derived key itself. The
        // caller may hand back a salt from an earlier response - the export of a multi-module
        // archive is one request per module, and every frame in one file has to share a key.
        std::pair<std::string, std::string> archiveKey(const std::string &passphrase, const std::string &saltBase64) {
            const auto salt = saltBase64.empty() ? Core::CryptoUtils::GenerateSalt(kSaltLength)
                                                 : Core::CryptoUtils::Base64Decode(saltBase64);
            if (salt.size() < kSaltLength) {
                throw std::runtime_error("salt is too short");
            }
            return {Core::CryptoUtils::Base64Encode(salt),
                    Core::CryptoUtils::DeriveKeyPbkdf2(passphrase, salt, kKdfIterations, kKeyLength)};
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
        const auto passphrase = Core::GetStringValue(jv, "passphrase");
        const auto salt = Core::GetStringValue(jv, "salt");

        if (all == !requestedModules.empty()) {
            return EmmServer::ErrorResponse(req, status::bad_request, "exactly one of \"all\" or \"modules\" is required");
        }

        // ekm is the one module whose export is worth as much as the database it came from:
        // Database::Entity::EKM::Key holds its material base64-encoded, not encrypted. It may
        // leave here, because a key store restored without its keys restores nothing - but only
        // inside a file that is worthless without a passphrase this process does not keep.
        const auto wantsEkm = all || std::ranges::find(requestedModules, "ekm") != requestedModules.end();
        if (wantsEkm && passphrase.empty()) {
            return EmmServer::ErrorResponse(req, status::bad_request,
                                            "exporting ekm requires a \"passphrase\": it carries the key material itself");
        }

        const auto &specs = moduleExportSpecs();
        std::vector<std::string> modules;
        if (all) {
            for (const auto &[name, spec]: specs) modules.push_back(name);
            std::ranges::sort(modules);
        } else {
            for (const auto &module: requestedModules) {
                if (!specs.contains(module)) {
                    // Listed from the specs rather than spelled out, so the message cannot fall
                    // behind the map the way a hand-written list already had.
                    std::vector<std::string> known;
                    for (const auto &[name, spec]: specs) known.push_back(name);
                    std::ranges::sort(known);
                    std::string expected;
                    for (const auto &name: known) expected += (expected.empty() ? "" : ", ") + name;
                    return EmmServer::ErrorResponse(req, status::bad_request, "unknown module '" + module + "' (expected one of: " + expected + ")");
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
        const auto exportedAt = Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now());

        if (!passphrase.empty()) {
            std::string saltBase64;
            std::string key;
            try {
                std::tie(saltBase64, key) = archiveKey(passphrase, salt);
            } catch (const std::exception &e) {
                return EmmServer::ErrorResponse(req, status::bad_request, std::string("cannot derive a key: ") + e.what());
            }

            // What is sealed is the same object an unencrypted export answers with, so opening a
            // frame yields a file the plain import path already understands.
            const auto payload = boost::json::serialize(boost::json::object{{"collections", std::move(collections)}});
            const auto sealed = Core::CryptoUtils::AesGcmEncrypt(key, payload);

            // The envelope around it stays readable: which modules, when, and how to derive the
            // key. Enough to tell what a file is, and to ask for the right passphrase, without
            // giving away a byte of what it holds.
            return EmmServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                                   {"encrypted", true},
                                                   {"modules", std::move(modulesJson)},
                                                   {"full", full},
                                                   {"exportedAt", exportedAt},
                                                   {"kdf", kKdfName},
                                                   {"iterations", kKdfIterations},
                                                   {"salt", saltBase64},
                                                   {"cipher", kCipherName},
                                                   {"data", Core::CryptoUtils::Base64Encode(sealed)},
                                           }));
        }

        return EmmServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"modules", std::move(modulesJson)},
                                               {"full", full},
                                               {"exportedAt", exportedAt},
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
        if (!jv.is_object()) {
            return EmmServer::ErrorResponse(req, status::bad_request, "expected a JSON object");
        }

        // A sealed archive arrives as the frames "export" handed out, one per module, plus the
        // passphrase to open them with. Each frame carries a whole export payload, so unsealing
        // them and merging the collections leaves exactly what the plain path below reads - the
        // rest of this handler cannot tell the difference, and neither can the caller.
        boost::json::object unsealed;
        if (Core::AttributeExists(jv, "frames")) {
            if (!jv.at("frames").is_array()) {
                return EmmServer::ErrorResponse(req, status::bad_request, "\"frames\" must be an array");
            }
            const auto passphrase = Core::GetStringValue(jv, "passphrase");
            if (passphrase.empty()) {
                return EmmServer::ErrorResponse(req, status::bad_request, "this export is encrypted: a \"passphrase\" is required");
            }
            std::string key;
            try {
                std::tie(std::ignore, key) = archiveKey(passphrase, Core::GetStringValue(jv, "salt"));
            } catch (const std::exception &e) {
                return EmmServer::ErrorResponse(req, status::bad_request, std::string("cannot derive a key: ") + e.what());
            }

            for (const auto &frame: jv.at("frames").as_array()) {
                try {
                    // GCM authenticates as it decrypts, so a wrong passphrase and a tampered file
                    // fail here in the same way - which is the point, and also why neither can be
                    // told apart in the message.
                    const auto plaintext = Core::CryptoUtils::AesGcmDecrypt(key, Core::CryptoUtils::Base64Decode(std::string(frame.as_string())));
                    const auto payload = boost::json::parse(plaintext);
                    for (const auto &[name, docs]: payload.at("collections").as_object()) {
                        unsealed[name] = docs;
                    }
                } catch (const std::exception &) {
                    return EmmServer::ErrorResponse(req, status::bad_request,
                                                    "cannot open this export: wrong passphrase, or the file has been altered");
                }
            }
            jv.as_object()["collections"] = std::move(unsealed);
        }

        if (!Core::AttributeExists(jv, "collections") || !jv.at("collections").is_object()) {
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
