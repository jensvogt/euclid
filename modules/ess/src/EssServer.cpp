//
// Created by vogje01 on 9/8/26.
//

// C++ includes
#include <optional>
#include <string>

// Euclid includes
#include <EssServer.h>

namespace Euclid::ESS {

    using Database::Entity::ESS::Secret;

    namespace {

        constexpr auto kServiceTimer = "ess-service-time";
        constexpr auto kServiceCounter = "ess-service-count";

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        // Key material, or the reason there is none. Mirrors ESM's bucket/object key lookup: a
        // secrets module that quietly carried on without a key would be storing plaintext, so
        // every failure here has to reach the caller as a refusal rather than a fallback.
        struct KeyLookup {
            std::string material;
            std::string ern;
            std::string error;
        };

        // The name of the key a namespace's secrets are encrypted under when nobody named one.
        // Per namespace, because an EKM key ERN carries the key's name and two namespaces sharing
        // one name would collide on it - and because development and production should not be
        // reading each other's secrets with the same key anyway.
        std::string defaultKeyName(const std::string &nameSpace) {
            return nameSpace.empty() ? "ess" : "ess-" + nameSpace;
        }

        // Key material for writing, by ERN. Refuses a key that cannot encrypt today, since storing
        // a secret under a key that is on its way out is how a value becomes unreadable later.
        KeyLookup writeKeyByErn(const std::string &keyErn) {

            const auto key = Database::RepositoryFactory::instance().ekmRepository()->findKeyByErn(keyErn);
            if (!key.has_value()) {
                return {.error = "No such key, keyErn: " + keyErn};
            }
            if (key->keyMaterial.empty()) {
                return {.error = "Key has no key material, keyErn: " + keyErn};
            }
            if (key->status != Database::Entity::EKM::KeyStatus::AVAILABLE) {
                return {.error = "Key is " + Database::Entity::EKM::KeyStatusToString(key->status) +
                                 " and can no longer encrypt, keyErn: " + keyErn};
            }
            return {.material = Core::CryptoUtils::Base64Decode(key->keyMaterial), .ern = key->ern};
        }

        // Key material for reading, by the ERN the secret recorded. Deliberately does not look at
        // the key's status: EKM blocks a revoked or pending-deletion key from encrypting anything
        // new but never from decrypting, precisely so that what was written under it stays
        // readable while it is migrated off.
        KeyLookup readKeyByErn(const std::string &keyErn) {

            const auto key = Database::RepositoryFactory::instance().ekmRepository()->findKeyByErn(keyErn);
            if (!key.has_value()) {
                return {.error = "This secret is encrypted under a key that no longer exists and cannot be read, keyErn: " + keyErn};
            }
            if (key->keyMaterial.empty()) {
                return {.error = "This secret is encrypted under a key that has no key material, keyErn: " + keyErn};
            }
            return {.material = Core::CryptoUtils::Base64Decode(key->keyMaterial), .ern = key->ern};
        }

        // The key a secret is encrypted under when the caller named none: the namespace's own,
        // created here the first time anything needs it.
        //
        // Created rather than refused, because the alternative interpretations are both worse. A
        // module that stored the value in the clear until somebody supplied a key would be a
        // secrets store that does not keep secrets, and one that refused every write until an
        // administrator had made a key by hand would send people back to putting passwords in
        // configuration files. The key is an ordinary EKM key: listable, rotatable, and the thing
        // to guard.
        KeyLookup namespaceKey(const std::string &accountId, const std::string &region, const std::string &nameSpace) {

            const auto repository = Database::RepositoryFactory::instance().ekmRepository();
            const auto name = defaultKeyName(nameSpace);

            if (const auto existing = repository->findKeyByName(accountId, nameSpace, name); existing.has_value()) {
                if (existing->keyMaterial.empty()) {
                    return {.error = "The namespace's secrets key has no key material, key: " + name};
                }
                // Without the status check the write side applies to a named key: this key belongs
                // to the module, and if somebody has revoked it there is nothing for the caller to
                // do about it - so it is used for reading and a new value is refused below only if
                // EKM itself will not have it.
                if (existing->status != Database::Entity::EKM::KeyStatus::AVAILABLE) {
                    return {.error = "The namespace's secrets key is " + Database::Entity::EKM::KeyStatusToString(existing->status) +
                                     " and can no longer encrypt, key: " + name +
                                     " - name another key with --key when storing this secret"};
                }
                return {.material = Core::CryptoUtils::Base64Decode(existing->keyMaterial), .ern = existing->ern};
            }

            Database::Entity::EKM::Key key;
            key.accountId = accountId;
            key.region = region;
            key.nameSpace = nameSpace;
            key.name = name;
            key.description = "Secrets of the '" + (nameSpace.empty() ? std::string("default") : nameSpace) + "' namespace, created by ESS";
            key.ern = Core::createEkmKeyErn(accountId, name);
            key.algorithm = "AES";
            key.length = 256;
            key.keyMaterial = Core::CryptoUtils::GenerateAes256Key();

            const auto stored = repository->upsertKey(key);
            log_info << "ESS created the namespace secrets key, key: " << stored.name << ", ern: " << stored.ern;

            return {.material = Core::CryptoUtils::Base64Decode(stored.keyMaterial), .ern = stored.ern};
        }

        std::string stringField(const boost::json::object &obj, const std::string &key, const std::string &fallback = "") {
            if (const auto *value = obj.if_contains(key); value && value->is_string()) return std::string(value->as_string());
            return fallback;
        }

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EssServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EssServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Whether this caller was given this secret, checked once a handler knows which one the
    // request is about. Nothing changes for a caller with no resource grants at all, which is
    // every human; this is for the technical principal an application runs as, deployed with the
    // one secret it needs, where the point is that a compromised application reaches that and
    // nothing else. It matters more here than anywhere: elsewhere a grant bounds what can be
    // changed, and here it bounds what can be read.
    static std::optional<response<string_body> > denyUngrantedSecret(const request<string_body> &req, const AuthResult &auth, const std::string &secretErn) {
        if (!auth.user.has_value()) return std::nullopt;
        if (EssServer::IsResourceAllowed(auth.user->userId, secretErn)) return std::nullopt;
        log_warning << "ESS resource denied, userId: " << auth.user->userId << ", secretErn: " << secretErn;
        return EssServer::ErrorResponse(req, status::forbidden, "Not authorized for this secret: " + secretErn);
    }

    // ── Action handlers ──────────────────────────────────────────────────────

    static response<string_body> handleCreateSecret(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-secret");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EssServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESS::CreateSecretRequest>(jv);
        if (request.name.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        if (request.name.starts_with("ern:")) {
            return EssServer::ErrorResponse(req, status::bad_request, "A secret is named rather than given as an ERN: " + request.name);
        }
        if (request.value.empty()) {
            // Refused rather than stored: an empty secret is almost always a caller whose shell ate
            // the value, and finding that out now beats finding it out when something reads it.
            return EssServer::ErrorResponse(req, status::bad_request, "value is required");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto ern = Core::createEssSecretErn(auth.user->accountId, ns, request.name);
        if (const auto denied = denyUngrantedSecret(req, auth, ern)) return *denied;

        const auto repository = Database::RepositoryFactory::instance().essRepository();
        if (repository->secretExists(auth.user->accountId, ns, request.name)) {
            // Not overwritten: create-secret quietly replacing a value would be a way to destroy
            // one by accident, and update-secret is the action that says it means to.
            return EssServer::ErrorResponse(req, status::conflict, "Secret exists already, name: " + request.name);
        }

        const auto key = request.keyErn.empty()
                                 ? namespaceKey(auth.user->accountId, auth.user->region, ns)
                                 : writeKeyByErn(request.keyErn);
        if (!key.error.empty()) {
            return EssServer::ErrorResponse(req, status::conflict, key.error);
        }

        Secret secret;
        secret.accountId = auth.user->accountId;
        secret.region = auth.user->region;
        secret.nameSpace = ns;
        secret.name = request.name;
        secret.description = request.description;
        secret.ern = ern;
        secret.encryptionKeyErn = key.ern;
        secret.version = 1;
        secret.rotated = std::chrono::system_clock::now();

        try {
            secret.value = Core::CryptoUtils::Base64Encode(Core::CryptoUtils::AesGcmEncrypt(key.material, request.value));
        } catch (const std::exception &ex) {
            return EssServer::ErrorResponse(req, status::internal_server_error, std::string("Could not encrypt the secret: ") + ex.what());
        }

        const auto stored = repository->upsertSecret(secret);
        log_info << "ESS secret created, name: " << stored.name << ", keyErn: " << stored.encryptionKeyErn;

        // Published like every other domain event - the name and who owns it, never the value.
        // Something that watches this is watching for a rotation it has to reload, not for the
        // secret itself.
        Database::EventBus::instance().Publish(
                "ess.secret.created",
                boost::json::value{
                        {"ern", stored.ern},
                        {"name", stored.name},
                        {"version", stored.version},
                        {"accountId", stored.accountId},
                        {"region", stored.region},
                        {"namespace", stored.nameSpace},
                },
                "ess");

        Dto::ESS::SecretResponse response;
        response.secret = Dto::ESS::EssMapper::toDto(stored);
        return EssServer::JsonResponse(req, status::ok, response.toJson());
    }

    // The one action that decrypts anything. Everything else in this module works on ciphertext it
    // never looks inside.
    static response<string_body> handleGetSecret(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-secret");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EssServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESS::SecretNameRequest>(jv);
        if (request.name.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "name is required");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto secret = Database::RepositoryFactory::instance().essRepository()->findSecretByName(auth.user->accountId, ns, request.name);
        if (!secret.has_value()) {
            return EssServer::ErrorResponse(req, status::not_found, "Secret not found, name: " + request.name);
        }
        if (const auto denied = denyUngrantedSecret(req, auth, secret->ern)) return *denied;

        const auto key = readKeyByErn(secret->encryptionKeyErn);
        if (!key.error.empty()) {
            return EssServer::ErrorResponse(req, status::conflict, key.error);
        }

        std::string value;
        try {
            value = Core::CryptoUtils::AesGcmDecrypt(key.material, Core::CryptoUtils::Base64Decode(secret->value));
        } catch (const std::exception &ex) {
            // The tag did not verify, which means the stored bytes are not what was written under
            // this key - a wrong key, or a value somebody edited in the database.
            log_error << "ESS could not decrypt a secret, name: " << secret->name << ", keyErn: " << secret->encryptionKeyErn << ", error: " << ex.what();
            return EssServer::ErrorResponse(req, status::internal_server_error, "The stored value could not be decrypted with the key it names");
        }

        // At info, because reading a secret is the event worth having in an audit trail - who, what
        // and when. The value is not part of that and never appears in a log line.
        log_info << "ESS secret read, name: " << secret->name << ", user: " << auth.user->userId << ", version: " << secret->version;

        Dto::ESS::GetSecretResponse response;
        response.secret = Dto::ESS::EssMapper::toDto(*secret);
        response.value = std::move(value);
        return EssServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListSecrets(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-secrets");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EssServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESS::ListSecretsRequest>(jv);
        const auto ns = std::string(req["x-euclid-namespace"]);

        const auto repository = Database::RepositoryFactory::instance().essRepository();
        auto secrets = repository->listSecrets(auth.user->accountId, ns, request.prefix, request.pageSize,
                                               request.pageIndex, request.sortColumn, request.sortDirection);

        // A principal restricted to particular secrets sees those and no others. Filtered rather
        // than refused, so an application listing what it may read gets an answer instead of a 403.
        std::erase_if(secrets, [&](const auto &secret) {
            return !EssServer::IsResourceAllowed(auth.user->userId, secret.ern);
        });

        Dto::ESS::ListSecretsResponse response;
        response.secrets = Dto::ESS::EssMapper::toDto(secrets);
        response.total = repository->countSecrets(auth.user->accountId, ns, request.prefix);

        return EssServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Rotation, a change of description, or a move to another key - the three things that can
    // happen to a secret that already exists. Only what the request names changes.
    static response<string_body> handleUpdateSecret(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "update-secret");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EssServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESS::UpdateSecretRequest>(jv);
        if (request.name.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        if (!request.hasValue && !request.hasDescription && request.keyErn.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "Name a value, a description or a key to change");
        }
        if (request.hasValue && request.value.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "value must not be empty");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repository = Database::RepositoryFactory::instance().essRepository();
        auto secret = repository->findSecretByName(auth.user->accountId, ns, request.name);
        if (!secret.has_value()) {
            return EssServer::ErrorResponse(req, status::not_found, "Secret not found, name: " + request.name);
        }
        if (const auto denied = denyUngrantedSecret(req, auth, secret->ern)) return *denied;

        if (request.hasDescription) secret->description = request.description;

        // A new key, a new value, or both - each of which means the stored bytes have to be
        // written again, and the one case where they do not is left alone entirely.
        const bool rekeying = !request.keyErn.empty() && request.keyErn != secret->encryptionKeyErn;
        if (request.hasValue || rekeying) {

            const auto key = rekeying ? writeKeyByErn(request.keyErn) : writeKeyByErn(secret->encryptionKeyErn);
            if (!key.error.empty()) {
                return EssServer::ErrorResponse(req, status::conflict, key.error);
            }

            std::string plaintext;
            if (request.hasValue) {
                plaintext = request.value;
            } else {
                // Re-keying without a new value: the value has to come back out under the old key
                // before it can go in under the new one. This is the only place the module reads a
                // value it was not asked for, and it never leaves the function.
                const auto oldKey = readKeyByErn(secret->encryptionKeyErn);
                if (!oldKey.error.empty()) {
                    return EssServer::ErrorResponse(req, status::conflict, oldKey.error);
                }
                try {
                    plaintext = Core::CryptoUtils::AesGcmDecrypt(oldKey.material, Core::CryptoUtils::Base64Decode(secret->value));
                } catch (const std::exception &ex) {
                    log_error << "ESS could not re-key a secret, name: " << secret->name << ", error: " << ex.what();
                    return EssServer::ErrorResponse(req, status::internal_server_error, "The stored value could not be decrypted with the key it names");
                }
            }

            try {
                secret->value = Core::CryptoUtils::Base64Encode(Core::CryptoUtils::AesGcmEncrypt(key.material, plaintext));
            } catch (const std::exception &ex) {
                return EssServer::ErrorResponse(req, status::internal_server_error, std::string("Could not encrypt the secret: ") + ex.what());
            }
            secret->encryptionKeyErn = key.ern;

            // Only a new value is a rotation. Moving the same value to another key changes how it
            // is protected, not what it is, and something waiting for the next rotation should not
            // be told one happened.
            if (request.hasValue) {
                secret->version += 1;
                secret->rotated = std::chrono::system_clock::now();
            }
        }

        const auto stored = repository->upsertSecret(*secret);
        log_info << "ESS secret updated, name: " << stored.name << ", version: " << stored.version
                 << ", keyErn: " << stored.encryptionKeyErn;

        if (request.hasValue) {
            Database::EventBus::instance().Publish(
                    "ess.secret.rotated",
                    boost::json::value{
                            {"ern", stored.ern},
                            {"name", stored.name},
                            {"version", stored.version},
                            {"accountId", stored.accountId},
                            {"region", stored.region},
                            {"namespace", stored.nameSpace},
                    },
                    "ess");
        }

        Dto::ESS::SecretResponse response;
        response.secret = Dto::ESS::EssMapper::toDto(stored);
        return EssServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteSecret(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-secret");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EssServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESS::SecretNameRequest>(jv);
        if (request.name.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "name is required");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repository = Database::RepositoryFactory::instance().essRepository();
        const auto secret = repository->findSecretByName(auth.user->accountId, ns, request.name);
        if (!secret.has_value()) {
            return EssServer::ErrorResponse(req, status::not_found, "Secret not found, name: " + request.name);
        }
        if (const auto denied = denyUngrantedSecret(req, auth, secret->ern)) return *denied;

        repository->deleteSecret(auth.user->accountId, ns, request.name);
        log_info << "ESS secret deleted, name: " << request.name << ", user: " << auth.user->userId;

        Database::EventBus::instance().Publish(
                "ess.secret.deleted",
                boost::json::value{
                        {"ern", secret->ern},
                        {"name", secret->name},
                        {"accountId", secret->accountId},
                        {"region", secret->region},
                        {"namespace", secret->nameSpace},
                },
                "ess");

        Dto::ESS::DeleteSecretResponse response;
        response.ern = secret->ern;
        response.name = secret->name;
        return EssServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Tags, for the same reason every other module has them: a secret outlives whoever created it,
    // and "which of these belong to the payroll system" has to be answerable without opening any
    // of them.
    static response<string_body> handleSecretTag(const request<string_body> &req, const bool add) {

        const auto action = add ? "add-secret-tag" : "delete-secret-tag";
        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", action);

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EssServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EssServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto name = stringField(obj, "name");
        const auto key = stringField(obj, "key");
        if (name.empty() || key.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "name and key are required");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repository = Database::RepositoryFactory::instance().essRepository();
        auto secret = repository->findSecretByName(auth.user->accountId, ns, name);
        if (!secret.has_value()) {
            return EssServer::ErrorResponse(req, status::not_found, "Secret not found, name: " + name);
        }
        if (const auto denied = denyUngrantedSecret(req, auth, secret->ern)) return *denied;

        if (add) {
            secret->tags[key] = stringField(obj, "value");
        } else {
            secret->tags.erase(key);
        }
        const auto stored = repository->upsertSecret(*secret);

        Dto::ESS::SecretResponse response;
        response.secret = Dto::ESS::EssMapper::toDto(stored);
        return EssServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EssServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "ESS action=" << action;

        if (action == "create-secret") return handleCreateSecret(req);
        if (action == "get-secret") return handleGetSecret(req);
        if (action == "list-secrets") return handleListSecrets(req);
        if (action == "update-secret") return handleUpdateSecret(req);
        if (action == "delete-secret") return handleDeleteSecret(req);
        if (action == "add-secret-tag") return handleSecretTag(req, true);
        if (action == "delete-secret-tag") return handleSecretTag(req, false);
        if (action == "get-metrics") return EssServer::MetricsResponse(req);

        log_warning << "Unknown action: " << action;
        return EssServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EssServer ────────────────────────────────────────────────────────────

    EssServer::EssServer(std::string socketPath, const int threads) : HttpActionServer("ESS", std::move(socketPath), threads) {}

    response<string_body> EssServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::ESS
