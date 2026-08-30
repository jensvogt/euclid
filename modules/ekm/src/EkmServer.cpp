// Euclid includes
#include <EkmServer.h>

#include "euclid/dto/ekm/mapper/EkmMapper.h"

namespace Euclid::EKM {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EkmServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        // Timer/counter names shared by every handler below - one series per action, labeled
        // "method"=<action>, e.g. name="queues-service-time" labelName="method" labelValue="send-message".
        constexpr auto kServiceTimer = "ekm-service-time";
        constexpr auto kServiceCounter = "ekm-service-count";
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EkmServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EkmServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Fills in the caller identity shared by every response DTO's "metadata" object. The
    // request ID that correlates this response with its request travels as the
    // "x-euclid-request-id" header instead (set centrally in HttpActionServer::JsonResponse).
    static void applyMetadata(Dto::BaseDto &response, const Database::Entity::EAM::User &user) {
        response.user = user.userId;
        response.accountId = user.accountId;
        response.region = user.region;
    }

    // ── Action handlers ──────────────────────────────────────────────────────
    // Each handler parses whatever fields it needs out of the JSON request body.
    // Return a fully formed HTTP response.

    static response<string_body> handleCreateKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-key");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::CreateKeyRequest>(jv);
        const auto ns = std::string(req["x-euclid-namespace"]);

        // Only AES-128/256 are supported for now - other algorithm/length combinations have no
        // key generation routine yet.
        std::string keyMaterial;
        if (request.algorithm == "AES" && request.length == 128) {
            keyMaterial = Core::CryptoUtils::GenerateAes128Key();
        } else if (request.algorithm == "AES" && request.length == 256) {
            keyMaterial = Core::CryptoUtils::GenerateAes256Key();
        } else {
            return EkmServer::ErrorResponse(req, status::bad_request,
                                             "Unsupported algorithm/length: " + request.algorithm + "/" + std::to_string(request.length));
        }

        // Keys are identified by a randomly generated ID rather than a user-chosen name (there's
        // no "name" field on CreateKeyRequest) - mirrors how EQS/ENS message IDs are minted.
        const auto keyId = Core::UuidUtils::CreateRandomUuid();

        Database::Entity::EKM::Key key;
        key.accountId = auth.user->accountId;
        key.region = auth.user->region;
        key.nameSpace = ns;
        key.name = keyId;
        key.ern = Core::createEkmKeyErn(auth.user->accountId, keyId);
        key.algorithm = request.algorithm;
        key.length = request.length;
        key.keyMaterial = keyMaterial;

        const auto saved = Database::RepositoryFactory::instance().ekmRepository()->upsertKey(key);

        Dto::EKM::CreateKeyResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Plaintext travels as the raw request body ("application/octet-stream") rather than a base64
    // JSON field - mirrors ESM's upload-part/download-part convention for payload-carrying actions.
    // The key ID travels as a header for the same reason: the body is opaque bytes, not JSON.
    static response<string_body> handleEncrypt(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "encrypt");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        const auto keyId = std::string(req["x-euclid-key-id"]);
        if (keyId.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-key-id header");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto key = Database::RepositoryFactory::instance().ekmRepository()->findKeyByName(auth.user->accountId, ns, keyId);
        if (!key.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Key not found, id: " + keyId);
        }
        if (key->keyMaterial.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Key '" + keyId + "' has no key material (it was created before key generation was added); create a new key");
        }

        std::string ciphertext;
        try {
            ciphertext = Core::CryptoUtils::AesGcmEncrypt(Core::CryptoUtils::Base64Decode(key->keyMaterial), req.body());
        } catch (const std::exception &ex) {
            return EkmServer::ErrorResponse(req, status::internal_server_error, std::string("Encryption failed: ") + ex.what());
        }

        response<string_body> res{status::ok, req.version()};
        res.set(field::content_type, "application/octet-stream");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(ciphertext);
        res.prepare_payload();
        return res;
    }

    // Mirror image of handleEncrypt(): ciphertext (IV || ciphertext || tag, as produced by
    // encrypt) travels as the raw request body, the key ID as a header, and the recovered
    // plaintext comes back as the raw response body.
    static response<string_body> handleDecrypt(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "decrypt");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        const auto keyId = std::string(req["x-euclid-key-id"]);
        if (keyId.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-key-id header");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto key = Database::RepositoryFactory::instance().ekmRepository()->findKeyByName(auth.user->accountId, ns, keyId);
        if (!key.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Key not found, id: " + keyId);
        }
        if (key->keyMaterial.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Key '" + keyId + "' has no key material (it was created before key generation was added); create a new key");
        }

        std::string plaintext;
        try {
            plaintext = Core::CryptoUtils::AesGcmDecrypt(Core::CryptoUtils::Base64Decode(key->keyMaterial), req.body());
        } catch (const std::exception &ex) {
            return EkmServer::ErrorResponse(req, status::bad_request, std::string("Decryption failed: ") + ex.what());
        }

        response<string_body> res{status::ok, req.version()};
        res.set(field::content_type, "application/octet-stream");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(plaintext);
        res.prepare_payload();
        return res;
    }

    static response<string_body> handleListKeys(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-keys");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::ListKeysRequest>(jv);
        log_info << "EKM ListKeys" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto nameSpace = std::string(req["x-euclid-namespace"]);
        const auto repo = Database::RepositoryFactory::instance().ekmRepository();
        const std::vector<Database::Entity::EKM::Key> keys = repo->listKeys(auth.user->accountId, nameSpace, request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "EKM key list, count: " << keys.size();

        Dto::EKM::ListKeysResponse response;
        response.keys = Dto::EKM::EkmMapper::toDto(keys);
        response.total = repo->countKeys(auth.user->accountId, nameSpace, request.prefix);

        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the EQS service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateKey,
            ListKeys,
            Encrypt,
            Decrypt
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-key") return Command::CreateKey;
        if (action == "list-keys") return Command::ListKeys;
        if (action == "encrypt") return Command::Encrypt;
        if (action == "decrypt") return Command::Decrypt;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EKM action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateKey:
                return handleCreateKey(req);

            case Command::ListKeys:
                return handleListKeys(req);

            case Command::Encrypt:
                return handleEncrypt(req);

            case Command::Decrypt:
                return handleDecrypt(req);

            case Command::Unknown:
            default:
                log_warning << "Unknown action: " << action;
                return EkmServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EkmServer ────────────────────────────────────────────────────────────

    EkmServer::EkmServer(std::string socketPath, const int threads) : HttpActionServer("EKM", std::move(socketPath), threads) {}

    EkmServer::~EkmServer() = default;

    response<string_body> EkmServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EQS