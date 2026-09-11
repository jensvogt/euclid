// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
            return EkmServer::ErrorResponse(req, status::bad_request, "Unsupported algorithm/length: " + request.algorithm + "/" + std::to_string(request.length));
        }

        // Keys are identified by a randomly generated ID rather than a user-chosen name (there's
        // no "name" field on CreateKeyRequest) - mirrors how EQS/ENS message IDs are minted.
        const auto keyId = Core::UuidUtils::CreateRandomUuid();

        Database::Entity::EKM::Key key;
        key.accountId = auth.user->accountId;
        key.region = auth.user->region;
        key.nameSpace = ns;
        key.name = keyId;
        key.description = request.description;
        key.ern = Core::createEkmKeyErn(auth.user->accountId, keyId);
        key.algorithm = request.algorithm;
        key.length = request.length;
        key.keyMaterial = keyMaterial;

        const auto saved = Database::RepositoryFactory::instance().ekmRepository()->upsertKey(key);

        // Published to the bus like every other domain event, so a client subscribed through EES
        // is told about a new key as it happens instead of polling list-keys - live over a
        // websocket, or durably if it asked for that.
        Database::EventBus::instance().Publish(
                "ekm.key.created",
                boost::json::value{
                        {"ern", saved.ern},
                        {"name", saved.name},
                        {"description", saved.description},
                        {"algorithm", saved.algorithm},
                        {"length", saved.length},
                        {"accountId", saved.accountId},
                        {"region", saved.region},
                },
                "ekm");

        Dto::EKM::CreateKeyResponse response;
        response.name = saved.name;
        response.description = saved.description;
        response.ern = saved.ern;
        // The algorithm and length come back too: a caller that let them default has otherwise no
        // answer to "what did I just create", and the ID alone does not say.
        response.algorithm = saved.algorithm;
        response.length = saved.length;
        response.status = Database::Entity::EKM::KeyStatusToString(saved.status);
        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Schedules a key for permanent deletion rather than deleting it outright: decryption stays
    // available until the background purge sweep (see EkmServer::EkmServer()) removes the key
    // once its deletionDate has passed. That grace period - pendingWindowInDays, 7 by default -
    // is what gives callers a chance to decrypt/migrate whatever was encrypted under this key
    // before it becomes unrecoverable. Encryption, however, is blocked immediately (status flips
    // to PENDING_DELETION) - a key that's on its way out shouldn't be gaining new data to lose.
    static response<string_body> handleDeleteKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-key");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::DeleteKeyRequest>(jv);
        if (request.keyId.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing keyId");
        }
        if (request.pendingWindowInDays < 1) {
            return EkmServer::ErrorResponse(req, status::bad_request, "pendingWindowInDays must be >= 1");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        auto key = Database::RepositoryFactory::instance().ekmRepository()->findKeyByName(auth.user->accountId, ns, request.keyId);
        if (!key.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Key not found, id: " + request.keyId);
        }

        key->deletionDate = std::chrono::system_clock::now() + std::chrono::hours(24 * request.pendingWindowInDays);
        key->status = Database::Entity::EKM::KeyStatus::PENDING_DELETION;
        const auto saved = Database::RepositoryFactory::instance().ekmRepository()->upsertKey(*key);

        log_info << "EKM key scheduled for deletion, id: " << saved.name << ", deletionDate: " << Core::DateTimeUtils::ToISO8601(saved.deletionDate);

        Dto::EKM::DeleteKeyResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        response.deletionDate = Core::DateTimeUtils::ToISO8601(saved.deletionDate);
        response.status = Database::Entity::EKM::KeyStatusToString(saved.status);
        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Permanently blocks encryption with this key while leaving decryption untouched - see
    // Database::Entity::EKM::KeyStatus. Unlike delete-key, revoking doesn't schedule the key for
    // removal; it stays around (and decryptable) indefinitely unless separately deleted. Refuses
    // to revoke a key that's already PENDING_DELETION: encryption is already blocked there, and
    // downgrading the status to REVOKED would hide the fact that a deletion is scheduled from
    // anything that reads status without also checking deletionDate.
    static response<string_body> handleRevokeKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "revoke-key");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::RevokeKeyRequest>(jv);
        if (request.ern.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing ERN");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        auto key = Database::RepositoryFactory::instance().ekmRepository()->findKeyByErn(request.ern);
        if (!key.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Key not found, ern: " + request.ern);
        }
        if (key->status == Database::Entity::EKM::KeyStatus::PENDING_DELETION) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Ern '" + request.ern + "' is already scheduled for deletion; encryption is already blocked");
        }

        key->status = Database::Entity::EKM::KeyStatus::REVOKED;
        const auto saved = Database::RepositoryFactory::instance().ekmRepository()->upsertKey(*key);

        log_info << "EKM key revoked, id: " << saved.name;

        Dto::EKM::RevokeKeyResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        response.status = Database::Entity::EKM::KeyStatusToString(saved.status);
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
        if (key->status != Database::Entity::EKM::KeyStatus::AVAILABLE) {
            return EkmServer::ErrorResponse(req, status::forbidden,
                                            "Key '" + keyId + "' is " + Database::Entity::EKM::KeyStatusToString(key->status) + " and cannot be used for encryption");
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

    // Replaces the free text recorded with a key to say what it is for. create-key takes a
    // description, and until now that was the only moment one could be set - so a key created
    // without one, or with the wrong one, carried it for the rest of its life.
    //
    // Nothing but that text changes: not the key material, not the status, not a date the key's
    // lifecycle turns on. Which is also why this is allowed whatever state the key is in - a
    // revoked key, or one already scheduled for deletion, is exactly the kind whose description is
    // worth correcting to say why.
    //
    // An empty description clears the key's own: the caller asked for the key to read that way, and
    // treating "" as "leave it alone" would leave no way to remove a description at all.
    static response<string_body> handleSetKeyDescription(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-key-description");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::SetKeyDescriptionRequest>(jv);
        if (request.ern.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing ERN");
        }

        const auto repo = Database::RepositoryFactory::instance().ekmRepository();
        auto key = repo->findKeyByErn(request.ern);
        if (!key.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Key not found, ern: " + request.ern);
        }

        key->description = request.description;
        const auto saved = repo->upsertKey(*key);

        log_info << "EKM key description set, id: " << saved.name << ", description: "
                 << (saved.description.empty() ? "(cleared)" : saved.description);

        Dto::EKM::SetKeyDescriptionResponse response;
        response.ern = saved.ern;
        response.name = saved.name;
        response.description = saved.description;
        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Upserts the tag unconditionally - mirrors add-topic-tag/add-queue-tag/add-bucket-tag in the
    // other modules (no set-key-tag counterpart exists here, so there's no "key must already have
    // this tag" variant to distinguish it from).
    static response<string_body> handleAddKeyTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-key-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::EKM::AddKeyTagRequest>(jv);
        log_info << "EKM AddKeyTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().ekmRepository();
        std::optional<Database::Entity::EKM::Key> foundKey = repo->findKeyByErn(ern);
        if (!foundKey.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Key not found, ern: " + ern);
        }
        foundKey->tags[key] = value;
        foundKey = repo->upsertKey(foundKey.value());

        return EkmServer::JsonResponse(req, status::ok);
    }

    // erase() is unconditional - a key key that doesn't have this tag silently no-ops rather than
    // 404ing, mirroring delete-queue-tag/delete-bucket-tag/delete-topic-tag.
    static response<string_body> handleDeleteKeyTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-key-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key] = boost::json::value_to<Dto::EKM::DeleteKeyTagRequest>(jv);
        log_info << "EKM DeleteKeyTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().ekmRepository();
        std::optional<Database::Entity::EKM::Key> foundKey = repo->findKeyByErn(ern);
        if (!foundKey.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Key not found, ern: " + ern);
        }
        foundKey->tags.erase(key);
        foundKey = repo->upsertKey(foundKey.value());

        return EkmServer::JsonResponse(req, status::ok);
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

    // ── Certificates ─────────────────────────────────────────────────────────
    // A certificate is key material like any other, so it lives here rather than as a pair of
    // files beside whichever listener uses it: one place that knows what exists, who it belongs to
    // and when it expires. What reads them back is the EAG module, whose HTTPS listeners name one.

    namespace {

        // Fills in everything the certificate says about itself, so those properties are answered
        // from the database afterwards rather than by parsing X.509 again on every list.
        // Returns false if the PEM is not a certificate at all, which is how a paste of the wrong
        // half of a key pair is caught here rather than at the next gateway start-up.
        bool applyCertificateInfo(Database::Entity::EKM::Certificate &certificate) {

            const auto info = Core::CertificateUtils::Inspect(certificate.certificatePem);
            if (!info.has_value()) return false;

            certificate.subject = info->subject;
            certificate.issuer = info->issuer;
            certificate.serialNumber = info->serialNumber;
            certificate.fingerprint = info->fingerprint;
            certificate.subjectAltNames = info->subjectAltNames;
            certificate.notBefore = info->notBefore;
            certificate.notAfter = info->notAfter;
            return true;
        }

        Database::Entity::EKM::Certificate newCertificate(const Database::Entity::EAM::User &user, const std::string &nameSpace,
                                                          const std::string &name, const std::string &description) {
            Database::Entity::EKM::Certificate certificate;
            certificate.accountId = user.accountId;
            certificate.region = user.region;
            certificate.nameSpace = nameSpace;
            certificate.name = name;
            certificate.description = description;
            certificate.ern = Core::createEkmCertificateErn(user.accountId, nameSpace, name);
            return certificate;
        }

        response<string_body> certificateResponse(const request<string_body> &req, const Database::Entity::EKM::Certificate &saved) {
            Dto::EKM::CertificateResponse response;
            response.certificate = Dto::EKM::EkmMapper::toDto(saved);
            return EkmServer::JsonResponse(req, status::ok, response.toJson());
        }

    }// namespace

    // Stores a certificate somebody else issued. Both halves are required and are checked against
    // each other: a certificate stored with a key that is not its own is accepted silently by
    // every step after this one, and only shows itself as a handshake that fails for every caller.
    static response<string_body> handleImportCertificate(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "import-certificate");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::ImportCertificateRequest>(jv);
        if (request.name.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing name");
        }
        if (request.certificate.empty() || request.privateKey.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Both certificate and privateKey are required");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        auto certificate = newCertificate(*auth.user, ns, request.name, request.description);
        certificate.certificatePem = request.certificate;
        certificate.privateKeyPem = request.privateKey;

        if (!applyCertificateInfo(certificate)) {
            return EkmServer::ErrorResponse(req, status::bad_request, "The certificate is not a PEM-encoded X.509 certificate");
        }
        if (!Core::CertificateUtils::KeyMatches(certificate.certificatePem, certificate.privateKeyPem)) {
            return EkmServer::ErrorResponse(req, status::bad_request, "The private key does not belong to the certificate");
        }

        const auto saved = Database::RepositoryFactory::instance().ekmRepository()->upsertCertificate(certificate);
        log_info << "EKM certificate imported, name: " << saved.name << ", subject: " << saved.subject
                 << ", notAfter: " << Core::DateTimeUtils::ToISO8601(saved.notAfter);

        Database::EventBus::instance().Publish(
                "ekm.certificate.imported",
                boost::json::value{
                        {"ern", saved.ern},
                        {"name", saved.name},
                        {"subject", saved.subject},
                        {"issuer", saved.issuer},
                        {"fingerprint", saved.fingerprint},
                        {"notAfter", Core::DateTimeUtils::ToISO8601(saved.notAfter)},
                        {"accountId", saved.accountId},
                        {"region", saved.region},
                },
                "ekm");

        return certificateResponse(req, saved);
    }

    // Generates a self-signed certificate, for an installation that has to serve HTTPS before
    // anybody has bought it a real one. Nobody has vouched for the result: the response says so
    // through "generated", and a client still has to be told to trust it.
    static response<string_body> handleCreateCertificate(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-certificate");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::CreateCertificateRequest>(jv);
        if (request.name.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing name");
        }

        // The certificate's own name when no common name was given: for a listener certificate
        // those are usually the same word, and a certificate with an empty subject is refused by
        // everything that reads it.
        const auto commonName = request.commonName.empty() ? request.name : request.commonName;

        const auto ns = std::string(req["x-euclid-namespace"]);
        auto certificate = newCertificate(*auth.user, ns, request.name, request.description);
        certificate.generated = true;

        try {
            const auto generated = Core::CertificateUtils::GenerateSelfSigned(commonName, request.subjectAltNames,
                                                                              request.validDays, static_cast<int>(request.keyBits));
            certificate.certificatePem = generated.certificate;
            certificate.privateKeyPem = generated.privateKey;
        } catch (const std::exception &ex) {
            return EkmServer::ErrorResponse(req, status::internal_server_error, std::string("Certificate generation failed: ") + ex.what());
        }
        applyCertificateInfo(certificate);

        const auto saved = Database::RepositoryFactory::instance().ekmRepository()->upsertCertificate(certificate);
        log_info << "EKM certificate created, name: " << saved.name << ", subject: " << saved.subject
                 << ", notAfter: " << Core::DateTimeUtils::ToISO8601(saved.notAfter);

        Database::EventBus::instance().Publish(
                "ekm.certificate.created",
                boost::json::value{
                        {"ern", saved.ern},
                        {"name", saved.name},
                        {"subject", saved.subject},
                        {"fingerprint", saved.fingerprint},
                        {"notAfter", Core::DateTimeUtils::ToISO8601(saved.notAfter)},
                        {"accountId", saved.accountId},
                        {"region", saved.region},
                },
                "ekm");

        return certificateResponse(req, saved);
    }

    static response<string_body> handleListCertificates(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-certificates");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::ListCertificatesRequest>(jv);
        const auto nameSpace = std::string(req["x-euclid-namespace"]);

        const auto repo = Database::RepositoryFactory::instance().ekmRepository();
        const auto certificates = repo->listCertificates(auth.user->accountId, nameSpace, request.prefix, request.pageSize,
                                                         request.pageIndex, request.sortColumn, request.sortDirection);

        Dto::EKM::ListCertificatesResponse response;
        response.certificates = Dto::EKM::EkmMapper::toDto(certificates);
        response.total = repo->countCertificates(auth.user->accountId, nameSpace, request.prefix);

        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetCertificate(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-certificate");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::CertificateNameRequest>(jv);
        if (request.name.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing name");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto certificate = Database::RepositoryFactory::instance().ekmRepository()->findCertificateByName(auth.user->accountId, ns, request.name);
        if (!certificate.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Certificate not found, name: " + request.name);
        }

        return certificateResponse(req, *certificate);
    }

    // Deleted outright, with no grace period: unlike a key, nothing becomes unreadable. A listener
    // already serving this certificate keeps the copy it loaded until it is restarted, which is
    // what makes deleting one recoverable - import a replacement under the same name.
    static response<string_body> handleDeleteCertificate(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-certificate");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::CertificateNameRequest>(jv);
        if (request.name.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing name");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repo = Database::RepositoryFactory::instance().ekmRepository();
        const auto certificate = repo->findCertificateByName(auth.user->accountId, ns, request.name);
        if (!certificate.has_value()) {
            return EkmServer::ErrorResponse(req, status::not_found, "Certificate not found, name: " + request.name);
        }

        repo->deleteCertificate(auth.user->accountId, ns, request.name);
        log_info << "EKM certificate deleted, name: " << request.name;

        Database::EventBus::instance().Publish(
                "ekm.certificate.deleted",
                boost::json::value{
                        {"ern", certificate->ern},
                        {"name", certificate->name},
                        {"accountId", certificate->accountId},
                        {"region", certificate->region},
                },
                "ekm");

        Dto::EKM::DeleteCertificateResponse response;
        response.ern = certificate->ern;
        response.name = certificate->name;
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
            Decrypt,
            DeleteKey,
            RevokeKey,
            SetKeyDescription,
            AddKeyTag,
            DeleteKeyTag,
            ImportCertificate,
            CreateCertificate,
            ListCertificates,
            GetCertificate,
            DeleteCertificate
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-key") return Command::CreateKey;
        if (action == "list-keys") return Command::ListKeys;
        if (action == "encrypt") return Command::Encrypt;
        if (action == "decrypt") return Command::Decrypt;
        if (action == "delete-key") return Command::DeleteKey;
        if (action == "revoke-key") return Command::RevokeKey;
        if (action == "set-key-description") return Command::SetKeyDescription;
        if (action == "add-key-tag") return Command::AddKeyTag;
        if (action == "delete-key-tag") return Command::DeleteKeyTag;
        if (action == "import-certificate") return Command::ImportCertificate;
        if (action == "create-certificate") return Command::CreateCertificate;
        if (action == "list-certificates") return Command::ListCertificates;
        if (action == "get-certificate") return Command::GetCertificate;
        if (action == "delete-certificate") return Command::DeleteCertificate;
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

            case Command::DeleteKey:
                return handleDeleteKey(req);

            case Command::RevokeKey:
                return handleRevokeKey(req);

            case Command::SetKeyDescription:
                return handleSetKeyDescription(req);

            case Command::AddKeyTag:
                return handleAddKeyTag(req);

            case Command::DeleteKeyTag:
                return handleDeleteKeyTag(req);

            case Command::ImportCertificate:
                return handleImportCertificate(req);

            case Command::CreateCertificate:
                return handleCreateCertificate(req);

            case Command::ListCertificates:
                return handleListCertificates(req);

            case Command::GetCertificate:
                return handleGetCertificate(req);

            case Command::DeleteCertificate:
                return handleDeleteCertificate(req);

            case Command::Unknown:
            default:
                log_warning << "Unknown action: " << action;
                return EkmServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EkmServer ────────────────────────────────────────────────────────────

    EkmServer::EkmServer(std::string socketPath, const int threads) : HttpActionServer("EKM", std::move(socketPath), threads) {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _purgeKeysTaskId = scheduler.SchedulePeriodic("ekm-purge-keys-pending-deletion", [] {
                                                          Database::RepositoryFactory::instance().ekmRepository()->purgeKeysPendingDeletion();
                                                      },
                                                      std::chrono::hours(1));
    }

    EkmServer::~EkmServer() {
        Core::Scheduler::instance().Cancel(_purgeKeysTaskId);
    }

    response<string_body> EkmServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EQS