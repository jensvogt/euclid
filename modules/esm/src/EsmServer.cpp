// Euclid includes
#include <EsmServer.h>

#include "euclid/dto/esm/AddBucketTagRequest.h"
#include "euclid/dto/esm/DeleteBucketTagRequest.h"

#include <thread>

namespace Euclid::ESM {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EsmServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        // Timer/counter names shared by every handler below - one series per action, labeled
        // "method"=<action>, e.g. name="queues-service-time" labelName="method" labelValue="send-message".
        constexpr auto kServiceTimer = "esm-service-time";
        constexpr auto kServiceCounter = "esm-service-count";

        // Fallback for "euclid.modules.storage.data-dir", matching the default in dist/etc/euclid*.json.
#ifdef _WIN32
        constexpr auto kDefaultDataDir = R"(C:\Program Files\euclid\data\esm)";
#else
        constexpr auto kDefaultDataDir = "/usr/local/euclid/data/storage";
#endif

        // Name of the per-upload file recording the upload's target bucket/key, written by
        // create-upload and read back by upload-part/complete-upload. Its presence also marks a
        // storage under .../uploads/ as a valid, still-open upload.
        constexpr auto kUploadMetaFile = "upload.json";

        // Directory a multipart upload's parts are staged in until the upload is completed or aborted.
        std::filesystem::path uploadDirFor(const std::string &uploadId) {
            const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
            return std::filesystem::path(dataDir) / "uploads" / uploadId;
        }

        // Name of the per-download file recording the object being downloaded, written by
        // create-download and read back by download-part/complete-download. Mirrors kUploadMetaFile,
        // but records where to read parts *from* (an existing object's internalName) rather than
        // where to write them to.
        constexpr auto kDownloadMetaFile = "download.json";

        // Directory a multipart download's metadata is staged in until the download is completed or aborted.
        std::filesystem::path downloadDirFor(const std::string &downloadId) {
            const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
            return std::filesystem::path(dataDir) / "downloads" / downloadId;
        }

        // Zero-padded so lexicographic storage-listing order matches numeric part order.
        std::string partFileName(const long partNumber) {
            std::ostringstream oss;
            oss << "part-" << std::setw(10) << std::setfill('0') << partNumber;
            return oss.str();
        }

        // Magic-byte sniffing only needs a small prefix of the file, not the whole thing - libmagic
        // itself only ever looks at the first few hundred bytes of whatever buffer it's given, so
        // reading more than this would just be wasted I/O against what can be a multi-GB assembled
        // object.
        constexpr std::size_t kContentTypeSniffBytes = 2000;

        // Determines a content type by sniffing the first kContentTypeSniffBytes bytes of the
        // assembled object with libmagic (Core::ContentTypeUtils), rather than trusting the key's
        // file extension.
        std::string contentTypeForFile(const std::filesystem::path &path) {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) return "application/octet-stream";

            std::string prefix(kContentTypeSniffBytes, '\0');
            file.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
            prefix.resize(static_cast<std::size_t>(file.gcount()));

            return Core::ContentTypeUtils::fromContent(prefix);
        }
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EsmServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EsmServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Whether this caller was given this bucket, checked once a handler knows which bucket the
    // request is actually about - the account and namespace were settled before it ran, but which
    // bucket inside them is named in a body or a header only the handler understands.
    //
    // Nothing changes for a caller with no resource grants at all, which is every human: this is
    // for principals deployed with a fixed set of buckets, where the point is that a compromised
    // application reaches those and nothing else.
    static std::optional<response<string_body> > denyUngrantedBucket(const request<string_body> &req, const AuthResult &auth, const std::string &bucketErn) {
        if (!auth.user.has_value()) return std::nullopt;
        if (EsmServer::IsResourceAllowed(auth.user->userId, bucketErn)) return std::nullopt;
        log_warning << "ESM resource denied, userId: " << auth.user->userId << ", bucketErn: " << bucketErn;
        return EsmServer::ErrorResponse(req, status::forbidden, "Not authorized for this bucket: " + bucketErn);
    }

    // Attributes travel as a header on put-object, because the body is the object's bytes and has
    // no room for anything else. The JSON is the same shape set-object-attributes takes, so a
    // client has one representation of an attribute map to produce rather than two.
    //
    // std::nullopt means the header was there but could not be read - reported as a bad request
    // rather than dropped, since silently storing an object without the metadata a caller asked
    // for is worse than refusing the upload.
    static std::optional<std::map<std::string, Database::Entity::COM::Variant> > attributesFromHeader(const request<string_body> &req) {

        const auto header = std::string(req["x-euclid-attributes"]);
        if (header.empty()) return std::map<std::string, Database::Entity::COM::Variant>{};

        try {
            const auto parsed = boost::json::parse(header);
            if (!parsed.is_object()) return std::nullopt;

            std::map<std::string, Database::Entity::COM::Variant> attributes;
            for (const auto &element: parsed.as_object()) {
                attributes[std::string(element.key())] = Dto::ESM::EsmMapper::toEntity(boost::json::value_to<Dto::COM::Variant>(element.value()));
            }
            return attributes;

        } catch (const std::exception &e) {
            log_warning << "ESM could not read x-euclid-attributes header, error: " << e.what();
            return std::nullopt;
        }
    }

    // ── Object events ────────────────────────────────────────────────────────
    // The three events that say what happened to an object, published from every path that changes
    // one: an upload, a multipart completion, a copy, a move, an attribute change, a delete, a
    // purge. Clients subscribe to these through EES, which is why they are emitted through one
    // function rather than written out at each call site - a listener that never hears about the
    // objects a purge removed holds a view of the bucket that is quietly wrong, and the way to
    // keep that from happening is for there to be a single place that says what an object event
    // is and a single list of the places that have to send one.
    //
    // The payload is flat, and its values are strings, numbers and booleans, because a
    // subscription filter matches a payload field by equality (EventBus::Publish). That is what
    // makes "every object in this bucket", "everything under this prefix" and "no directory
    // markers" expressible without a subscriber receiving everything and discarding most of it.
    // "accountId" is the field that decides an event is never stored for another account at all,
    // so it is always present.
    constexpr auto kObjectCreated = "esm.object.created";
    constexpr auto kObjectUpdated = "esm.object.updated";
    constexpr auto kObjectDeleted = "esm.object.deleted";

    // userId is who asked for the change, which is not always the object's owner - a move made by
    // an operator does not change who uploaded the thing being moved, and a listener usually wants
    // to know both.
    static void publishObjectEvent(const std::string &eventType, const Database::Entity::ESM::Object &object,
                                   const std::optional<Database::Entity::ESM::Bucket> &bucket, const std::string &userId) {

        // A key is a path by convention only. A subscriber watching one "directory" has to be able
        // to name it, so that convention is spelled out once, here, instead of by each of them.
        const auto slash = object.key.rfind('/');
        const auto prefix = slash == std::string::npos ? std::string() : object.key.substr(0, slash + 1);

        // Falls back to the bucket's account for objects written before ESM recorded one per
        // object: an event with no accountId reaches every subscriber of every account, which is
        // the one failure here that would not be visible as a missing event.
        const auto accountId = !object.accountId.empty() ? object.accountId
                               : bucket.has_value()      ? bucket->accountId
                                                         : std::string();

        Database::EventBus::instance().Publish(
                eventType,
                boost::json::value{
                        {"ern", object.ern},
                        {"bucketErn", object.bucketErn},
                        {"bucketName", bucket.has_value() ? bucket->name : std::string()},
                        {"key", object.key},
                        {"prefix", prefix},
                        {"directory", Database::Entity::ESM::IsDirectoryKey(object.key)},
                        {"size", object.size},
                        {"contentType", object.contentType},
                        {"md5Sum", object.md5Sum},
                        {"owner", object.owner},
                        {"userId", userId},
                        {"accountId", accountId},
                        {"region", object.region},
                        {"namespace", object.nameSpace},
                        {"eventTime", Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())}},
                "esm");
    }

    // ── Bucket subscriptions ─────────────────────────────────────────────────
    // Fans out to every subscription of bucketErn - one EventBus event per subscription, so a
    // subscribing instance (any one of them, via the claim mechanism) delivers it. Type SQS goes
    // straight to an EQS queue (event "esm.subscription.delivery", consumed by EqsServer's
    // handleSubscriptionDelivery); type SNS goes to an ENS topic (event
    // "esm.subscription.publication", consumed by EnsServer's handleObjectPublishedNotification,
    // which publishes it as a regular topic message and lets ENS's own subscription fan-out take
    // it from there).
    //
    // These two are named for the delivery rather than for the object because that is what they
    // are: each carries a queue or topic to put a notification into, and is addressed to the one
    // module that can do it. The object events above are the domain events, which anything - a
    // module, an application - may subscribe to.
    static void notifyBucketSubscriptions(const std::string &bucketErn, const std::string &key, const std::string &ern, const long size, const std::string &contentType, const std::string &md5Sum) {

        const auto subscriptions = Database::RepositoryFactory::instance().esmRepository()->listSubscriptionsBySourceErn(bucketErn);
        if (subscriptions.empty()) return;

        const boost::json::value notification = {
                {"eventType", "esm:ObjectCreated:Put"},
                {"bucketErn", bucketErn},
                {"key", key},
                {"ern", ern},
                {"size", size},
                {"contentType", contentType},
                {"md5Sum", md5Sum},
        };
        const auto body = boost::json::serialize(notification);

        for (const auto &subscription: subscriptions) {
            if (subscription.type == "SQS") {
                const boost::json::value payload = {
                        {"messageId", Core::UuidUtils::CreateRandomUuid()},
                        {"sourceErn", bucketErn},
                        {"targetErn", subscription.targetErn},
                        {"body", body},
                };
                Database::EventBus::instance().Publish("esm.subscription.delivery", payload, "esm");
            } else if (subscription.type == "SNS") {
                const boost::json::value payload = {
                        {"sourceErn", bucketErn},
                        {"targetErn", subscription.targetErn},
                        {"body", body},
                };
                Database::EventBus::instance().Publish("esm.subscription.publication", payload, "esm");
            }
        }
    }

    // ── Action handlers ──────────────────────────────────────────────────────
    // Each handler parses whatever fields it needs out of the JSON request body.
    // Return a fully formed HTTP response.

    response<string_body> EsmServer::handleCreateBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-bucket");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CreateBucketRequest>(jv);

        const auto ns = std::string(req["x-euclid-namespace"]);

        Database::Entity::ESM::Bucket bucket;
        bucket.name = request.name;
        bucket.ern = Core::createEsmBucketErn(auth.user->accountId, ns, request.name);
        bucket.region = auth.user->region;
        bucket.accountId = auth.user->accountId;
        bucket.nameSpace = ns;
        bucket.owner = auth.user->userId;

        const auto saved = Database::RepositoryFactory::instance().esmRepository()->upsertBucket(bucket);
        log_info << "ESM bucket created, ern: " << bucket.ern;

        // accountId is what keeps this event inside the account it belongs to: EventBus stores an
        // event that names no account for every subscriber of every account, so a bucket name
        // would otherwise be visible across a shared installation.
        Database::EventBus::instance().Publish(
                "esm.bucket.modified",
                boost::json::value{{"ern", saved.ern}, {"name", saved.name},
                                   {"accountId", saved.accountId}, {"region", saved.region}},
                "esm");

        Dto::ESM::CreateBucketResponse response;
        response.name = saved.name;
        response.ern = saved.ern;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    response<string_body> EsmServer::handleListBuckets(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-buckets");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ListBucketsRequest>(jv);
        const auto ns = std::string(req["x-euclid-namespace"]);

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const std::vector<Database::Entity::ESM::Bucket> buckets = repo->listBuckets(auth.user->accountId, ns, request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "ESM bucket list, count: " << buckets.size();

        Dto::ESM::ListBucketsResponse response;
        response.buckets = Dto::ESM::EsmMapper::toDto(buckets);
        response.total = repo->countBuckets(auth.user->accountId, ns, request.prefix);

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    response<string_body> EsmServer::handleGetBucketErn(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-bucket-ern");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::GetBucketErnRequest>(jv);

        const std::optional<Database::Entity::ESM::Bucket> bucket = Database::RepositoryFactory::instance().esmRepository()->findBucketByName(request.name);
        log_debug << "EMS bucket ERN, name: " << request.name << ", ern: " << (bucket.has_value() ? bucket->ern : "(none)");

        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, name: " + request.name);
        }

        Dto::ESM::GetBucketErnResponse response;
        response.ern = bucket->ern;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    response<string_body> EsmServer::handleGetBucketSize(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-bucket-size");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::GetBucketSizeRequest>(jv);

        const std::optional<Database::Entity::ESM::Bucket> bucket = Database::RepositoryFactory::instance().esmRepository()->findBucketByErn(request.ern);
        log_debug << "ESM bucket size, ern: " << request.ern << ", size: " << bucket->size;

        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.ern);
        }

        Dto::ESM::GetBucketSizeResponse response;
        response.ern = bucket->ern;
        response.size = bucket->size;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    response<string_body> EsmServer::handleDeleteBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-bucket");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ESM::DeleteBucketRequest::fromJson(req.body());
        log_info << "ESM bucket deleted, ern: " << request.ern;

        // Read before it is gone: the event says which bucket and whose, and after the delete
        // there is nothing left to say it with - the same reason delete-object looks its object up
        // first.
        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const auto bucket = repo->findBucketByErn(request.ern);

        repo->deleteBucketByErn(request.ern);

        Database::EventBus::instance().Publish(
                "esm.bucket.deleted",
                boost::json::value{{"ern", request.ern},
                                   {"name", bucket.has_value() ? bucket->name : std::string()},
                                   {"accountId", bucket.has_value() ? bucket->accountId : std::string()},
                                   {"region", bucket.has_value() ? bucket->region : std::string()}},
                "esm");

        return EsmServer::JsonResponse(req, status::ok);
    }

    // Stores a small object in a single request/response round trip, skipping the
    // create-upload/upload-part/complete-upload dance entirely. Worthwhile for objects that fit
    // under the client's chosen part size, where multipart buys nothing (there's only ever one
    // part) but still costs three requests plus staging-then-assembling-then-discarding a scratch
    // file for no reason. Internal to the CLI/Java client's upload path - "upload-file" picks
    // between this and the multipart flow based on the source file's size, invisibly to the caller
    // either way.
    //
    // Like upload-part, this does NOT conform to the JSON request convention used elsewhere -
    // bucketErn/key travel as headers and the body is the object's raw bytes - but unlike
    // upload-part its *response* is still JSON: reuses Dto::ESM::CompleteUploadResponse as-is,
    // since "here's the object that now exists" has the same shape whether it was assembled from
    // multipart parts or written in one shot (status here is "COMPLETED" rather than "UPLOADED",
    // since there's no post-processing left to do once this returns - content type/MD5 are already
    // known).
    // Renaming a bucket is not an edit of one field. A bucket's name is in its ERN, every object
    // carries that ERN, and every object's own ERN carries the bucket's name as well - so the name
    // is written in as many places as the bucket has objects, and all of them have to move
    // together or the bucket stops being findable from its contents.
    //
    // What this deliberately does not do is repoint the things outside ESM that name the bucket.
    // A transfer server points at a bucket ERN, and rewriting another module's definition from
    // here would leave ETS with a record it never agreed to - so a bucket a transfer server is
    // serving is refused instead, and the operator moves the server with "ets update-server".
    response<string_body> EsmServer::handleRenameBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "rename-bucket");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::RenameBucketRequest>(jv);
        if (request.ern.empty() || request.newName.empty()) {
            return ErrorResponse(req, status::bad_request, "ern and newName are required");
        }

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const auto bucket = repo->findBucketByErn(request.ern);
        if (!bucket.has_value()) {
            return ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.ern);
        }
        if (bucket->accountId != auth.user->accountId) {
            return ErrorResponse(req, status::forbidden, "Bucket does not belong to the caller's account");
        }
        if (const auto denied = denyUngrantedBucket(req, auth, request.ern)) return *denied;

        if (bucket->name == request.newName) {
            return ErrorResponse(req, status::bad_request, "The bucket already has that name");
        }
        // Refused rather than merged: two buckets cannot share a name, and an operator who meant
        // to move objects between them has copy-object and move-object for that.
        if (repo->findBucketByName(request.newName).has_value()) {
            return ErrorResponse(req, status::conflict, "Bucket already exists, name: " + request.newName);
        }

        // A transfer server names the bucket it serves, and its clients are mid-session; changing
        // the bucket underneath it would leave uploads going to an ERN that no longer exists.
        for (const auto servers = Database::RepositoryFactory::instance().etsRepository()->listServers("");
             const auto &server: servers) {
            if (server.bucketErn == request.ern) {
                return ErrorResponse(req, status::conflict,
                                      "Bucket is served by transfer server '" + server.serverId
                                              + "'; stop it or point it at another bucket first");
            }
        }

        const auto oldErn = bucket->ern;
        const auto oldName = bucket->name;
        const auto newErn = Core::createEsmBucketErn(bucket->accountId, bucket->nameSpace, request.newName);

        // The bucket first: if anything below fails, the objects still point at a bucket that
        // exists under its new ERN, which is recoverable by running the rename again. The reverse
        // order would leave objects pointing at an ERN nothing answers to.
        const auto stored = repo->renameBucket(oldErn, request.newName, newErn);
        if (!stored.has_value()) {
            return ErrorResponse(req, status::internal_server_error, "Could not rename bucket, ern: " + oldErn);
        }

        const auto objects = repo->renameBucketObjects(oldErn, newErn, oldName, request.newName);

        // Subscriptions name the bucket they watch, and a subscription left on the old ERN would
        // simply stop delivering - silently, since nothing publishes under that ERN any more.
        const auto subscriptions = repo->repointSubscriptions(oldErn, newErn);

        log_info << "ESM bucket renamed, from: " << oldName << ", to: " << request.newName
                << ", objects: " << objects << ", subscriptions: " << subscriptions;

        Database::EventBus::instance().Publish(
                "esm.bucket.modified",
                boost::json::value{{"ern", stored->ern}, {"name", stored->name}, {"previousErn", oldErn},
                                   {"previousName", oldName}, {"accountId", stored->accountId}, {"region", stored->region}},
                "esm");

        Dto::ESM::RenameBucketResponse response;
        response.name = stored->name;
        response.ern = stored->ern;
        response.objects = objects;
        response.subscriptions = subscriptions;

        return JsonResponse(req, status::ok, response.toJson());
    }

    response<string_body> EsmServer::handlePutObject(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "put-object");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        const auto bucketErn = std::string(req["x-euclid-bucket-ern"]);
        if (bucketErn.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-bucket-ern header");
        }
        const auto key = std::string(req["x-euclid-key"]);
        if (key.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-key header");
        }
        const auto attributes = attributesFromHeader(req);
        if (!attributes.has_value()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "Malformed x-euclid-attributes header");
        }

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const auto bucket = repo->findBucketByErn(bucketErn);
        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + bucketErn);
        }
        if (const auto denied = denyUngrantedBucket(req, auth, bucketErn)) return *denied;

        const auto &data = req.body();

        // Looked up before writing so a re-upload to the same key can be recognized (and the file
        // it replaces cleaned up below), same as complete-upload's existingObject.
        const auto existingObject = repo->findObjectByBucketAndKey(bucketErn, key);
        const auto internalName = Core::UuidUtils::CreateRandomUuid();
        const auto ern = Core::createEsmObjectErn(auth.user->accountId, bucket->nameSpace, bucket->name + "/" + key);

        const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        if (ec) {
            log_error << "ESM could not create storage data directory, path: " << dataDir << ", error: " << ec.message();
            return EsmServer::ErrorResponse(req, status::internal_server_error, "Could not store object");
        }

        const std::filesystem::path destPath = std::filesystem::path(dataDir) / internalName;
        {
            std::ofstream dest(destPath, std::ios::binary | std::ios::trunc);
            if (!dest.is_open()) {
                return EsmServer::ErrorResponse(req, status::internal_server_error, "Could not write object");
            }
            dest.write(data.data(), static_cast<std::streamsize>(data.size()));
        }

        // Small enough to hash/sniff inline rather than handing off to a background thread the way
        // complete-upload does for a potentially huge assembled file - this whole handler only
        // exists for objects under the client's part size in the first place.
        const auto md5Sum = Core::CryptoUtils::md5SumFile(destPath.string());
        const auto contentType = contentTypeForFile(destPath);

        Database::Entity::ESM::Object object;
        if (existingObject) object.oid = existingObject->oid;
        object.bucketErn = bucketErn;
        object.key = key;
        object.internalName = internalName;
        object.ern = ern;
        object.owner = auth.user->userId;
        object.region = auth.user->region;
        object.accountId = auth.user->accountId;
        object.nameSpace = std::string(req["x-euclid-namespace"]);
        object.size = static_cast<long>(data.size());
        object.status = Database::Entity::ESM::ObjectStatus::COMPLETED;
        object.contentType = contentType;
        object.md5Sum = md5Sum;
        object.attributes = *attributes;
        repo->upsertObject(object);

        // A directory is not one of the bucket's objects as far as its counters are concerned -
        // it holds no bytes and is not something a client stored, so counting it would report a
        // bucket as fuller than what a listing shows.
        if (auto freshBucket = repo->findBucketByErn(bucketErn); freshBucket.has_value()) {
            freshBucket->size += object.size;
            if (!Database::Entity::ESM::IsDirectoryKey(key)) freshBucket->objects++;
            freshBucket = repo->upsertBucket(*freshBucket);
            log_debug << "Updated bucket, ern: " << freshBucket->ern << ", size: " << freshBucket->size << ", objects: " << freshBucket->objects;
        }

        // A re-upload to the same key replaces the DB row above; drop the now-unreferenced old file.
        if (existingObject && !existingObject->internalName.empty() && existingObject->internalName != internalName) {
            std::error_code oldEc;
            std::filesystem::remove(std::filesystem::path(dataDir) / existingObject->internalName, oldEc);
            if (oldEc)
                log_warning << "Could not remove superseded object file, internalName: " << existingObject->internalName << ", error: " << oldEc.message();
        }

        log_info << "ESM put object, bucket: " << bucketErn << ", key: " << key << ", internalName: " << internalName << ", size: " << data.size();

        // Uploading to a key that already held an object replaces it, which is an update of that
        // key rather than a second creation of it - a listener that treats "created" as "this key
        // is new" should not be told twice.
        publishObjectEvent(existingObject ? kObjectUpdated : kObjectCreated, object, bucket, auth.user->userId);
        notifyBucketSubscriptions(bucketErn, key, ern, object.size, contentType, md5Sum);

        Dto::ESM::CompleteUploadResponse response;
        response.bucketErn = bucketErn;
        response.key = key;
        response.ern = ern;
        response.size = object.size;
        response.status = Database::Entity::ESM::ObjectStatusToString(Database::Entity::ESM::ObjectStatus::COMPLETED);
        response.contentType = contentType;
        response.md5Sum = md5Sum;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Starts a multipart upload: stages a scratch storage on disk that the "upload-part" action
    // will later copy each part into, keyed by an upload ID the caller carries for the lifetime of
    // the upload. Internal to the create-upload/upload-part/complete-upload workflow used by the
    // CLI/Java client, rather than a bucket-management action in its own right.
    response<string_body> EsmServer::handleCreateUpload(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-upload");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CreateUploadRequest>(jv);

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const auto bucket = repo->findBucketByErn(request.bucketErn);
        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.bucketErn);
        }
        if (const auto denied = denyUngrantedBucket(req, auth, request.bucketErn)) return *denied;

        // Seeds the object row with status CREATED right away, so its lifecycle is observable
        // from the very start of the upload rather than only appearing once complete-upload
        // finishes. upload-part/complete-upload advance status via the same bucketErn+key key.
        // Starts from any existing object at this key (a re-upload) rather than a blank one, so
        // its internalName/ern/size - and thus the still-valid previous file on disk - survive
        // until complete-upload actually replaces them.
        Database::Entity::ESM::Object object;
        const auto existing = repo->findObjectByBucketAndKey(request.bucketErn, request.key);
        if (existing.has_value()) {
            object = *existing;
        }
        object.bucketErn = request.bucketErn;
        object.key = request.key;
        // The object collection carries a non-sparse unique index on "ern", so a row seeded without
        // one holds ern:"" - and any second upload created while the first is still CREATED collides
        // on that empty value with a duplicate-key error, which surfaces as a 500 on create-upload.
        // Assigning the ERN here (same derivation complete-upload uses) keeps every in-flight upload
        // distinct under the index. Only filled when empty, so a re-upload keeps the ERN its still
        // valid previous version was published under until complete-upload replaces it.
        if (object.ern.empty()) {
            object.ern = Core::createEsmObjectErn(auth.user->accountId, bucket->nameSpace, bucket->name + "/" + request.key);
        }
        object.owner = auth.user->userId;
        object.region = auth.user->region;
        object.accountId = auth.user->accountId;
        object.nameSpace = std::string(req["x-euclid-namespace"]);
        object.status = Database::Entity::ESM::ObjectStatus::CREATED;
        repo->upsertObject(object);

        const auto uploadId = Core::UuidUtils::CreateRandomUuid();
        const auto uploadDir = uploadDirFor(uploadId);

        std::error_code ec;
        std::filesystem::create_directories(uploadDir, ec);
        if (ec) {
            log_error << "ESM could not create upload, path: " << uploadDir.string() << ", error: " << ec.message();
            return EsmServer::ErrorResponse(req, status::internal_server_error, "Could not create upload storage");
        }

        // Records the upload's target bucket/key alongside the staged parts, so "complete-upload"
        // can assemble them into the right place using only the upload ID the client carries.
        // "replaces" is recorded here because complete-upload cannot work it out for itself: the
        // row seeded above means it always finds an object at this key, whether or not one was
        // there before the upload started - and the difference is exactly what decides whether it
        // publishes a created or an updated event.
        const boost::json::value meta = {
                {"bucketErn", request.bucketErn},
                {"key", request.key},
                {"owner", auth.user->userId},
                {"replaces", existing.has_value()},
                {"created", Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())},
        };
        std::ofstream metaFile(uploadDir / kUploadMetaFile);
        metaFile << boost::json::serialize(meta);
        metaFile.close();

        log_info << "ESM upload created, id: " << uploadId << ", path: " << uploadDir.string();

        Dto::ESM::CreateUploadResponse response;
        response.uploadId = uploadId;
        response.bucketErn = request.bucketErn;
        response.key = request.key;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Stores one part of an in-progress multipart upload. Internal to the create-upload/
    // upload-part/complete-upload workflow; the CLI's user-facing "upload-file" action is the only
    // caller for the CLI, splitting a local file into parts and calling this once per part.
    //
    // This handler does NOT conform to the JSON in/out convention every other action here uses:
    // uploadId/partNumber travel as "x-euclid-upload-id"/"x-euclid-part-number" headers instead of
    // JSON fields, and the request body is the part's raw bytes ("application/octet-stream")
    // rather than base64 wrapped in JSON. Parts are typically the bulk of an upload's bytes and
    // this action is never called by anything outside the CLI/Java client, so there's no external
    // consumer relying on a JSON schema here - skipping base64 (~33% smaller payload, no
    // encode/decode pass) is a straightforward win for large-file upload throughput.
    response<string_body> EsmServer::handleUploadPart(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "upload-part");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        const auto uploadId = std::string(req["x-euclid-upload-id"]);
        if (uploadId.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-upload-id header");
        }

        long partNumber = 0;
        try {
            partNumber = std::stol(std::string(req["x-euclid-part-number"]));
        } catch (const std::exception &) {
            return EsmServer::ErrorResponse(req, status::bad_request, "Missing or invalid x-euclid-part-number header");
        }

        const auto uploadDir = uploadDirFor(uploadId);
        if (!std::filesystem::exists(uploadDir / kUploadMetaFile)) {
            return EsmServer::ErrorResponse(req, status::not_found, "Upload not found, id: " + uploadId);
        }

        // Advances the object's status from CREATED to UPLOADING on the first part received.
        // Guarded on the current status so only the first of what can be thousands of parts on a
        // large upload triggers a write; every part still pays one indexed lookup, since the
        // bucketErn/key needed to find the object row live only in the upload's meta file.
        {
            std::ifstream metaFile(uploadDir / kUploadMetaFile);
            std::ostringstream buffer;
            buffer << metaFile.rdbuf();
            const auto meta = boost::json::parse(buffer.str());
            const auto bucketErn = std::string(meta.at("bucketErn").as_string());
            const auto key = std::string(meta.at("key").as_string());

            const auto repo = Database::RepositoryFactory::instance().esmRepository();
            if (auto object = repo->findObjectByBucketAndKey(bucketErn, key);
                object.has_value() && object->status == Database::Entity::ESM::ObjectStatus::CREATED) {
                object->status = Database::Entity::ESM::ObjectStatus::UPLOADING;
                repo->upsertObject(*object);
            }
        }

        const auto &data = req.body();

        std::ofstream partFile(uploadDir / partFileName(partNumber), std::ios::binary | std::ios::trunc);
        if (!partFile.is_open()) {
            return EsmServer::ErrorResponse(req, status::internal_server_error, "Could not write upload part");
        }
        partFile.write(data.data(), static_cast<std::streamsize>(data.size()));
        partFile.close();

        log_debug << "ESM upload part, id: " << uploadId << ", part: " << partNumber << ", size: " << data.size();

        Dto::ESM::UploadPartResponse response;
        response.uploadId = uploadId;
        response.partNumber = partNumber;
        response.size = static_cast<long>(data.size());

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Assembles a completed multipart upload's staged parts into the final object under the
    // bucket's storage storage, then discards the upload's scratch storage. Internal to the
    // create-upload/upload-part/complete-upload workflow; called by the CLI's "upload-file" action
    // once all parts have been uploaded.
    response<string_body> EsmServer::handleCompleteUpload(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "complete-upload");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CompleteUploadRequest>(jv);
        log_info << "ESM CompleteUpload, id: " << request.uploadId;

        // Same header put-object takes: an upload big enough to be split into parts must not lose
        // the metadata a small one keeps.
        const auto attributes = attributesFromHeader(req);
        if (!attributes.has_value()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "Malformed x-euclid-attributes header");
        }

        const auto uploadDir = uploadDirFor(request.uploadId);
        const auto metaPath = uploadDir / kUploadMetaFile;
        if (!std::filesystem::exists(metaPath)) {
            return EsmServer::ErrorResponse(req, status::not_found, "Upload not found, id: " + request.uploadId);
        }

        boost::json::value meta;
        {
            std::ifstream metaFile(metaPath);
            std::ostringstream buffer;
            buffer << metaFile.rdbuf();
            meta = boost::json::parse(buffer.str());
        }
        const auto bucketErn = std::string(meta.at("bucketErn").as_string());
        const auto key = std::string(meta.at("key").as_string());
        // Written by create-upload; absent from an upload started by an older ESM, in which case
        // the object counts as new - which is what it is in all but the overwrite case.
        const auto *replacesValue = meta.is_object() ? meta.as_object().if_contains("replaces") : nullptr;
        const auto replaces = replacesValue != nullptr && replacesValue->is_bool() && replacesValue->as_bool();

        const auto repo = Database::RepositoryFactory::instance().esmRepository();

        const auto bucket = repo->findBucketByErn(bucketErn);
        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + bucketErn);
        }
        // Checked here rather than at create-upload only: the upload's parts are already staged,
        // but nothing has been written into the bucket yet, and this is the call that would.
        if (const auto denied = denyUngrantedBucket(req, auth, bucketErn)) return *denied;

        // Zero-padded part filenames sort lexicographically in numeric order.
        std::vector<std::filesystem::path> parts;
        for (const auto &entry: std::filesystem::directory_iterator(uploadDir)) {
            if (entry.path().filename().string().starts_with("part-")) {
                parts.push_back(entry.path());
            }
        }
        std::ranges::sort(parts);

        if (parts.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "Upload has no parts, id: " + request.uploadId);
        }

        // Objects live in a flat storage named after a freshly generated UUID rather than
        // under a bucket/key path - the database (looked up below by bucketErn+key) is the only
        // place that mapping is recorded, so a bare storage listing can't be used to browse or
        // resolve objects by key.
        const auto existingObject = repo->findObjectByBucketAndKey(bucketErn, key);
        const auto internalName = Core::UuidUtils::CreateRandomUuid();
        const auto ern = Core::createEsmObjectErn(auth.user->accountId, bucket->nameSpace, bucket->name + "/" + key);

        // Cheap (stat-only, no reads) so it can be reported in the response below without waiting
        // for the background pass to actually assemble the file.
        std::size_t totalSize = 0;
        for (const auto &partPath: parts) totalSize += std::filesystem::file_size(partPath);

        // All parts are in (that's what calling complete-upload means) but post-processing -
        // assembly, MD5, content-type detection - hasn't run yet. Marking UPLOADED now and
        // returning immediately makes that in-between window observable instead of blocking the
        // caller (and the gateway worker thread handling them) for as long as a potentially huge
        // file takes to assemble and hash; a background thread below does that work and advances
        // the object to COMPLETED once it's actually done.
        if (existingObject) {
            Database::Entity::ESM::Object uploaded = *existingObject;
            uploaded.status = Database::Entity::ESM::ObjectStatus::UPLOADED;
            repo->upsertObject(uploaded);
        }

        const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
        const std::filesystem::path destPath = std::filesystem::path(dataDir) / internalName;
        const auto owner = auth.user->userId;
        const auto region = auth.user->region;
        const auto accountId = auth.user->accountId;
        const auto ns = std::string(req["x-euclid-namespace"]);

        // Detached rather than joined: Dispatch() must return promptly so the gateway worker
        // thread handling this request isn't tied up for as long as a multi-GB file takes to
        // assemble and hash. Everything it touches is captured by value (paths, strings, the
        // existingObject snapshot) since req and the variables above go out of scope once this
        // handler returns. The whole body is wrapped in try/catch: an exception escaping a
        // detached thread's entry function calls std::terminate() and takes down the entire
        // process, unlike an exception in a normal request handler which route()/Dispatch() would
        // otherwise catch.
        std::thread([repo, uploadDir, parts, dataDir, destPath, internalName, ern, bucketErn, bucket, key, owner, region, accountId, ns, existingObject, replaces, attributes = *attributes, uploadId = request.uploadId] {
            try {
                std::error_code ec;
                std::filesystem::create_directories(dataDir, ec);
                if (ec) {
                    log_error << "Could not create storage data storage, path: " << dataDir << ", error: " << ec.message();
                    return;
                }

                std::size_t assembledSize = 0;
                {
                    std::ofstream dest(destPath, std::ios::binary | std::ios::trunc);
                    if (!dest.is_open()) {
                        log_error << "Could not write object, upload id: " << uploadId << ", path: " << destPath.string();
                        return;
                    }
                    for (const auto &partPath: parts) {
                        std::ifstream part(partPath, std::ios::binary);
                        dest << part.rdbuf();
                        assembledSize += std::filesystem::file_size(partPath);
                    }
                }

                std::filesystem::remove_all(uploadDir, ec);
                if (ec)
                    log_warning << "Could not remove upload storage, path: " << uploadDir.string() << ", error: " << ec.message();

                // Post-processing: MD5 the assembled file and sniff its content type from its
                // first bytes before marking the object COMPLETED.
                const auto md5Sum = Core::CryptoUtils::md5SumFile(destPath.string());
                const auto contentType = contentTypeForFile(destPath);

                Database::Entity::ESM::Object object;
                if (existingObject) object.oid = existingObject->oid;
                object.bucketErn = bucketErn;
                object.key = key;
                object.internalName = internalName;
                object.ern = ern;
                object.owner = owner;
                object.region = region;
                object.accountId = accountId;
                object.nameSpace = ns;
                object.size = static_cast<long>(assembledSize);
                object.status = Database::Entity::ESM::ObjectStatus::COMPLETED;
                object.contentType = contentType;
                object.md5Sum = md5Sum;
                object.attributes = attributes;
                repo->upsertObject(object);

                // Re-fetches the bucket rather than reusing the snapshot from before assembly
                // started - post-processing can take a while for a large file, so that snapshot
                // may be stale by now. Still starts from a real bucket (not a default-constructed
                // one): upsertBucket() keys on ern, and upserting a blank one would create/
                // accumulate into a separate phantom bucket instead of updating this one.
                if (auto freshBucket = repo->findBucketByErn(bucketErn); freshBucket.has_value()) {
                    freshBucket->size += object.size;
                    if (!Database::Entity::ESM::IsDirectoryKey(key)) freshBucket->objects++;
                    freshBucket = repo->upsertBucket(*freshBucket);
                    log_debug << "Updated bucket, ern: " << freshBucket->ern << ", size: " << freshBucket->size << ", objects: " << freshBucket->objects;
                }

                // A re-upload to the same key replaces the DB row above; drop the now-unreferenced old file.
                if (existingObject && !existingObject->internalName.empty() && existingObject->internalName != internalName) {
                    std::error_code oldEc;
                    std::filesystem::remove(std::filesystem::path(dataDir) / existingObject->internalName, oldEc);
                    if (oldEc)
                        log_warning << "Could not remove superseded object file, internalName: " << existingObject->internalName << ", error: " << oldEc.message();
                }

                log_info << "Completed upload, id: " << uploadId << ", key: " << key << ", internalName: " << internalName << ", size: " << assembledSize;

                // Published here, at the end of the background pass, rather than when the handler
                // accepted the upload: until the file is assembled and hashed the object exists
                // only as an UPLOADED row, and a listener that fetched it then would get an
                // incomplete object.
                publishObjectEvent(replaces ? kObjectUpdated : kObjectCreated, object, bucket, owner);
                notifyBucketSubscriptions(bucketErn, key, ern, object.size, contentType, md5Sum);
            } catch (const std::exception &e) {
                log_error << "Post-processing failed, upload id: " << uploadId << ", error: " << e.what();
            } catch (...) {
                log_error << "Post-processing failed, upload id: " << uploadId << ", unknown error";
            }
        }).detach();

        log_info << "Accepted upload, id: " << request.uploadId << ", key: " << key << ", size: " << totalSize << " - post-processing in background";

        Dto::ESM::CompleteUploadResponse response;
        response.bucketErn = bucketErn;
        response.key = key;
        response.ern = ern;
        response.size = static_cast<long>(totalSize);
        response.status = Database::Entity::ESM::ObjectStatusToString(Database::Entity::ESM::ObjectStatus::UPLOADED);

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Mirrors handlePutObject() for downloads: returns a small object's full bytes in a single
    // request/response round trip, skipping the create-download/download-part/complete-download
    // sequence entirely. Internal to the CLI/Java client's download path - "download-file" tries
    // this first and falls back to the multipart flow only if it comes back too large.
    //
    // Unlike create-download, this needs no session/scratch state at all - it looks the object up
    // by bucket/key directly, same as put-object does for the write side. The size cutoff is
    // enforced HERE rather than by the caller: since a download's size isn't known until asked
    // (unlike an upload, where the caller already has the local file stat'd), the caller declares
    // its part size as "x-euclid-part-size" and an object at or above that size is rejected with
    // HTTP 413 rather than streamed back - telling the caller to fall back to the multipart flow
    // instead of this handler ever risking an unbounded response body.
    //
    // Like download-part, the response body is raw bytes ("application/octet-stream"), not JSON.
    response<string_body> EsmServer::handleGetObject(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-object");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        const auto bucketErn = std::string(req["x-euclid-bucket-ern"]);
        if (bucketErn.empty()) {
            return ErrorResponse(req, status::bad_request, "Missing x-euclid-bucket-ern header");
        }
        const auto key = std::string(req["x-euclid-key"]);
        if (key.empty()) {
            return ErrorResponse(req, status::bad_request, "Missing x-euclid-key header");
        }

        long maxInlineSize = 0;
        try {
            maxInlineSize = std::stol(std::string(req["x-euclid-part-size"]));
        } catch (const std::exception &) {
            return ErrorResponse(req, status::bad_request, "Missing or invalid x-euclid-part-size header");
        }
        if (maxInlineSize < 1) {
            return ErrorResponse(req, status::bad_request, "x-euclid-part-size must be >= 1");
        }

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        if (const auto denied = denyUngrantedBucket(req, auth, bucketErn)) return *denied;

        const auto object = repo->findObjectByBucketAndKey(bucketErn, key);
        if (!object.has_value()) {
            return ErrorResponse(req, status::not_found, "Object not found, bucket: " + bucketErn + ", key: " + key);
        }
        if (object->status != Database::Entity::ESM::ObjectStatus::COMPLETED) {
            return ErrorResponse(req, status::conflict, "Object is not available for download, status: " + Database::Entity::ESM::ObjectStatusToString(object->status));
        }
        if (object->size >= maxInlineSize) {
            return ErrorResponse(req, status::payload_too_large, "Object is too large for a single-request download, size: " + std::to_string(object->size));
        }

        const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
        std::ifstream in(std::filesystem::path(dataDir) / object->internalName, std::ios::binary);
        if (!in.is_open()) {
            return ErrorResponse(req, status::internal_server_error, "Could not open object file for download, bucket: " + bucketErn + ", key: " + key);
        }

        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string data = buffer.str();

        log_info << "ESM get object, bucket: " << bucketErn << ", key: " << key << ", size: " << data.size();

        response<string_body> res{status::ok, req.version()};
        res.set(field::content_type, "application/octet-stream");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(data);
        res.prepare_payload();
        return res;
    }

    // Starts a multipart download: the mirror image of handleCreateUpload() - rather than staging
    // an empty scratch storage for the caller to fill via upload-part, it looks up the already-
    // completed object being downloaded and stages a meta file recording where download-part
    // should read bytes *from*, keyed by a download ID the caller carries for the lifetime of the
    // download.
    response<string_body> EsmServer::handleCreateDownload(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-download");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CreateDownloadRequest>(jv);

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const auto object = repo->findObjectByBucketAndKey(request.bucketErn, request.key);
        if (!object.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Object not found, bucket: " + request.bucketErn + ", key: " + request.key);
        }
        if (object->status != Database::Entity::ESM::ObjectStatus::COMPLETED) {
            return EsmServer::ErrorResponse(req, status::conflict, "Object is not available for download, status: " + Database::Entity::ESM::ObjectStatusToString(object->status));
        }

        const auto downloadId = Core::UuidUtils::CreateRandomUuid();
        const auto downloadDir = downloadDirFor(downloadId);

        std::error_code ec;
        std::filesystem::create_directories(downloadDir, ec);
        if (ec) {
            log_error << "ESM could not create download, path: " << downloadDir.string() << ", error: " << ec.message();
            return EsmServer::ErrorResponse(req, status::internal_server_error, "Could not create download storage");
        }

        // Records the object being downloaded so download-part can serve byte ranges using only
        // the download ID the client carries, mirroring how create-upload's meta file lets
        // upload-part/complete-upload resolve the target bucket/key.
        const boost::json::value meta = {
                {"bucketErn", object->bucketErn},
                {"key", object->key},
                {"internalName", object->internalName},
                {"size", object->size},
                {"owner", auth.user->userId},
                {"created", Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())},
        };
        std::ofstream metaFile(downloadDir / kDownloadMetaFile);
        metaFile << boost::json::serialize(meta);
        metaFile.close();

        log_info << "ESM download created, id: " << downloadId << ", path: " << downloadDir.string();

        Dto::ESM::CreateDownloadResponse response;
        response.downloadId = downloadId;
        response.bucketErn = object->bucketErn;
        response.key = object->key;
        response.ern = object->ern;
        response.size = object->size;
        response.contentType = object->contentType;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Serves one byte-range part of an in-progress multipart download. Internal to the
    // create-download/download-part/complete-download workflow; the CLI's "download-file" action
    // is the only caller, requesting one part per <part-size> chunk of the object.
    //
    // The mirror image of handleUploadPart(): download ID/part number/part size travel as headers
    // (there's nothing worth putting in a JSON request body here), but where upload-part reads raw
    // bytes from the request body, download-part *writes* raw bytes to the response body -
    // "application/octet-stream", not JSON - for the same reason upload-part skips base64: parts
    // are typically the bulk of a download's bytes.
    response<string_body> EsmServer::handleDownloadPart(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "download-part");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        const auto downloadId = std::string(req["x-euclid-download-id"]);
        if (downloadId.empty()) {
            return ErrorResponse(req, status::bad_request, "Missing x-euclid-download-id header");
        }

        long partNumber = 0;
        try {
            partNumber = std::stol(std::string(req["x-euclid-part-number"]));
        } catch (const std::exception &) {
            return ErrorResponse(req, status::bad_request, "Missing or invalid x-euclid-part-number header");
        }
        if (partNumber < 1) {
            return ErrorResponse(req, status::bad_request, "x-euclid-part-number must be >= 1");
        }

        long partSize = 0;
        try {
            partSize = std::stol(std::string(req["x-euclid-part-size"]));
        } catch (const std::exception &) {
            return ErrorResponse(req, status::bad_request, "Missing or invalid x-euclid-part-size header");
        }
        if (partSize < 1) {
            return ErrorResponse(req, status::bad_request, "x-euclid-part-size must be >= 1");
        }

        const auto downloadDir = downloadDirFor(downloadId);
        if (!std::filesystem::exists(downloadDir / kDownloadMetaFile)) {
            return ErrorResponse(req, status::not_found, "Download not found, id: " + downloadId);
        }

        boost::json::value meta;
        {
            std::ifstream metaFile(downloadDir / kDownloadMetaFile);
            std::ostringstream buffer;
            buffer << metaFile.rdbuf();
            meta = boost::json::parse(buffer.str());
        }
        const auto internalName = std::string(meta.at("internalName").as_string());
        const auto totalSize = meta.at("size").as_int64();

        const auto offset = (partNumber - 1) * partSize;
        if (offset >= totalSize) {
            return ErrorResponse(req, status::bad_request, "Part number beyond end of object, id: " + downloadId + ", part: " + std::to_string(partNumber));
        }

        const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
        std::ifstream in(std::filesystem::path(dataDir) / internalName, std::ios::binary);
        if (!in.is_open()) {
            return ErrorResponse(req, status::internal_server_error, "Could not open object file for download, id: " + downloadId);
        }
        in.seekg(offset);

        const auto readSize = std::min<std::int64_t>(partSize, totalSize - offset);
        std::string data(static_cast<std::size_t>(readSize), '\0');
        in.read(data.data(), static_cast<std::streamsize>(readSize));
        data.resize(static_cast<std::size_t>(in.gcount()));

        log_debug << "ESM download part, id: " << downloadId << ", part: " << partNumber << ", size: " << data.size();

        response<string_body> res{status::ok, req.version()};
        res.set(field::content_type, "application/octet-stream");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(data);
        res.prepare_payload();
        return res;
    }

    // Discards a completed (or abandoned) download's scratch meta storage. The mirror image of
    // handleCompleteUpload(), but far simpler - a download doesn't assemble or mutate anything, so
    // there's no post-processing step, just cleanup.
    response<string_body> EsmServer::handleCompleteDownload(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "complete-download");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ESM::CompleteDownloadRequest::fromJson(req.body());
        log_info << "ESM CompleteDownload, id: " << request.downloadId;

        const auto downloadDir = downloadDirFor(request.downloadId);
        std::error_code ec;
        std::filesystem::remove_all(downloadDir, ec);
        if (ec)
            log_warning << "Could not remove download storage, path: " << downloadDir.string() << ", error: " << ec.message();

        return JsonResponse(req, status::ok);
    }

    response<string_body> EsmServer::handleListObjects(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-objects");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ListObjectsRequest>(jv);
        log_info << "ESM ListObjects, bucket: " << request.bucketErn << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto repo = Database::RepositoryFactory::instance().esmRepository();

        // A syntactically valid bucketErn from another account must not leak that bucket's
        // object listing - verify the bucket actually belongs to the caller's account first.
        const auto bucket = repo->findBucketByErn(request.bucketErn);
        if (!bucket.has_value()) {
            return ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.bucketErn);
        }
        if (bucket->accountId != auth.user->accountId) {
            return ErrorResponse(req, status::forbidden, "Bucket does not belong to the caller's account");
        }
        if (const auto denied = denyUngrantedBucket(req, auth, request.bucketErn)) return *denied;

        const std::vector<Database::Entity::ESM::Object> objects = repo->listObjects(request.bucketErn, request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection, request.includeDirectories);
        log_info << "ESM got object list, bucket: " << request.bucketErn << ", count: " << objects.size();

        Dto::ESM::ListObjectsResponse response;
        response.objects = Dto::ESM::EsmMapper::toDto(objects);
        response.total = repo->countObjects(request.bucketErn, request.prefix, request.includeDirectories);

        return JsonResponse(req, status::ok, response.toJson());
    }

    response<string_body> EsmServer::handleGetObjectCount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-object-count");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::GetObjectCountRequest>(jv);

        const std::optional<Database::Entity::ESM::Bucket> bucket = Database::RepositoryFactory::instance().esmRepository()->findBucketByErn(request.bucketErn);

        if (!bucket.has_value()) {
            return ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.bucketErn);
        }
        log_info << "ESM get object count, ern: " << request.bucketErn << ", count: " << bucket->objects;

        Dto::ESM::GetObjectCountResponse response;
        response.ern = bucket->ern;
        response.count = bucket->objects;

        return JsonResponse(req, status::ok, response.toJson());
    }

    response<string_body> EsmServer::handleDeleteObject(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-object");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ESM::DeleteObjectRequest::fromJson(req.body());
        log_info << "Storage DeleteObject, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().esmRepository();

        // Looked up before deleting so the bucket's aggregate size/objects can be adjusted -
        // without this, a bucket's stats would only ever grow, never reflecting deletions. The
        // object is also what the event below is made of, and after the delete there is nothing
        // left to describe it with.
        const auto object = repo->findObjectByErn(request.ern);
        std::optional<Database::Entity::ESM::Bucket> bucket;
        if (object.has_value()) {
            // Addressed by object ERN, so which bucket it belongs to is only known now - and that
            // is what a grant is written in terms of.
            if (const auto denied = denyUngrantedBucket(req, auth, object->bucketErn)) return *denied;
            bucket = repo->findBucketByErn(object->bucketErn);
            if (bucket.has_value()) {
                bucket->size = std::max<long>(0, bucket->size - object->size);
                // Mirrors put-object: a directory was never counted, so removing one must not
                // decrement anything either.
                if (!Database::Entity::ESM::IsDirectoryKey(object->key)) bucket->objects = std::max<long>(0, bucket->objects - 1);
                repo->upsertBucket(*bucket);
            }

            const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(dataDir) / object->internalName, ec);
            if (ec)
                log_warning << "Could not remove object file, internalName: " << object->internalName << ", error: " << ec.message();
        }

        repo->deleteObjectByErn(request.ern);

        // No event for a delete of something that was not there: an ERN that named no object is a
        // caller's mistake, not a change to the bucket.
        if (object.has_value()) publishObjectEvent(kObjectDeleted, *object, bucket, auth.user->userId);

        return JsonResponse(req, status::ok);
    }

    // Removes every object of a bucket, e.g. so the (now empty) bucket can be deleted. Reuses the
    // same per-object disk cleanup as handleDeleteObject() rather than calling it directly, since
    // looking the object back up by ERN for each one would be wasted work when listObjects() already
    // has it.
    response<string_body> EsmServer::handlePurgeBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-bucket");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ESM::PurgeBucketRequest::fromJson(req.body());
        log_info << "ESM PurgeBucket, ern: " << request.bucketErn;

        // Repository connection
        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        auto bucket = repo->findBucketByErn(request.bucketErn);
        if (!bucket.has_value()) {
            return ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.bucketErn);
        }
        if (const auto denied = denyUngrantedBucket(req, auth, request.bucketErn)) return *denied;
        // Directories are listed here, unlike everywhere else: a purge that left them behind
        // would empty a bucket that still could not be deleted. They are not counted below,
        // though, since they were never counted when they were created.
        const auto objects = repo->listObjects(request.bucketErn, request.prefix, -1, -1, "", "asc", true);

        const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
        long purgedSize = 0;
        long purgedObjects = 0;
        for (const auto &object: objects) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(dataDir) / object.internalName, ec);
            if (ec)
                log_warning << "Could not remove object file, internalName: " << object.internalName << ", error: " << ec.message();
            repo->deleteObjectByErn(object.ern);
            purgedSize += object.size;
            if (!Database::Entity::ESM::IsDirectoryKey(object.key)) purgedObjects++;

            // One event per object, the same as if each had been deleted on its own. A purge is
            // the cheapest way to make a listener's view of a bucket wrong, and "the bucket was
            // purged" would not tell it which of the objects it was tracking are gone - so it
            // pays for a publish per object, which is the same order of work as the delete and
            // the file removal it already does for each one.
            publishObjectEvent(kObjectDeleted, object, bucket, auth.user->userId);
        }
        log_info << "ESM bucket purged, ern: " << request.bucketErn << ", count: " << purgedObjects;

        // Adjust counters by what was actually deleted rather than zeroing them out - a prefix-scoped
        // purge only removes some of the bucket's objects, so anything left outside the prefix must
        // still be reflected.
        bucket->size = std::max<long>(0, bucket->size - purgedSize);
        bucket->objects = std::max<long>(0, bucket->objects - purgedObjects);
        bucket = repo->upsertBucket(bucket.value());
        log_debug << "ESM bucket updated, ern: " << request.bucketErn << ", count: " << bucket->objects << ", size: " << bucket->size;

        Dto::ESM::PurgeBucketResponse response;
        response.ern = request.bucketErn;
        response.count = purgedObjects;

        return JsonResponse(req, status::ok, response.toJson());
    }

    // Attributes are the one part of an object a client owns outright - everything else about it
    // (size, content type, checksum) is either the bytes it uploaded or something ESM derived from
    // them, and none of it is writable. They live behind their own actions rather than being part
    // of put-object because they outlive any single upload: re-uploading the bytes at a key is a
    // new object as far as the store is concerned, and would otherwise silently drop them.
    //
    // The four actions follow the add/set/list/delete split every other keyed collection in
    // euclid uses (bucket tags, queue tags): adding refuses a name that is already there and
    // setting refuses one that is not, so a caller that mistypes a name is told about it instead
    // of quietly creating a second attribute or overwriting a first.

    namespace {

        // Everything the four attribute actions need before touching an object: the object itself,
        // or the response explaining why the caller cannot have it.
        struct ResolvedObject {
            std::optional<Database::Entity::ESM::Object> object;
            // Carried out with it because it was looked up here anyway (the account check below
            // is what needs it), and an object event names the bucket the object is in.
            std::optional<Database::Entity::ESM::Bucket> bucket;
            std::optional<response<string_body> > error;
        };

        ResolvedObject resolveObject(const request<string_body> &req, const AuthResult &auth, const std::string &ern) {

            if (ern.empty()) {
                return {.error = EsmServer::ErrorResponse(req, status::bad_request, "ern is required")};
            }

            const auto repo = Database::RepositoryFactory::instance().esmRepository();
            auto object = repo->findObjectByErn(ern);
            if (!object.has_value()) {
                return {.error = EsmServer::ErrorResponse(req, status::not_found, "Object not found, ern: " + ern)};
            }

            // Same check list-objects makes: a syntactically valid ERN from another account must
            // not expose - or let anyone write to - that account's objects.
            auto bucket = repo->findBucketByErn(object->bucketErn);
            if (!bucket.has_value()) {
                return {.error = EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + object->bucketErn)};
            }
            if (bucket->accountId != auth.user->accountId) {
                return {.error = EsmServer::ErrorResponse(req, status::forbidden, "Object does not belong to the caller's account")};
            }
            if (!EsmServer::IsResourceAllowed(auth.user->userId, object->bucketErn)) {
                return {.error = EsmServer::ErrorResponse(req, status::forbidden, "Not authorized for this bucket: " + object->bucketErn)};
            }

            return {.object = std::move(object), .bucket = std::move(bucket)};
        }

    }// namespace

    // copy-object, move-object and rename-object, which are three ways of asking for the same two
    // decisions: does the target get its own bytes, and does the source survive.
    //
    // A move does not touch the bytes at all. An object's data lives in a flat file named after
    // its internalName, and only the database row says which key that file answers to - so moving
    // is a row edit, whatever the object's size. A copy is the one that has to duplicate the file,
    // because the two objects have to be able to outlive each other: deleting either one removes
    // the file it names.
    static response<string_body> transferObject(const request<string_body> &req, const std::string &action,
                                                const std::string &sourceBucketErn, const std::string &sourceKey,
                                                const std::string &targetBucketErn, const std::string &targetKey,
                                                const bool keepSource) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", action);

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        if (sourceBucketErn.empty() || sourceKey.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "source bucket and key are required");
        }
        if (targetBucketErn.empty() || targetKey.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "target bucket and key are required");
        }
        if (sourceBucketErn == targetBucketErn && sourceKey == targetKey) {
            return EsmServer::ErrorResponse(req, status::bad_request, "source and target are the same object");
        }

        const auto repo = Database::RepositoryFactory::instance().esmRepository();

        const auto sourceBucket = repo->findBucketByErn(sourceBucketErn);
        if (!sourceBucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + sourceBucketErn);
        }
        const auto targetBucket = repo->findBucketByErn(targetBucketErn);
        if (!targetBucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + targetBucketErn);
        }

        // Both ends are checked: reading out of a bucket and writing into one are separate
        // permissions, and this action does both.
        if (sourceBucket->accountId != auth.user->accountId || targetBucket->accountId != auth.user->accountId) {
            return EsmServer::ErrorResponse(req, status::forbidden, "Bucket does not belong to the caller's account");
        }
        if (const auto denied = denyUngrantedBucket(req, auth, sourceBucketErn)) return *denied;
        if (const auto denied = denyUngrantedBucket(req, auth, targetBucketErn)) return *denied;

        const auto source = repo->findObjectByBucketAndKey(sourceBucketErn, sourceKey);
        if (!source.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Object not found, bucket: " + sourceBucketErn + ", key: " + sourceKey);
        }

        // Refused rather than silently replacing: an operator who meant to overwrite can delete
        // the target first and say so, and one who mistyped a key gets told.
        if (repo->findObjectByBucketAndKey(targetBucketErn, targetKey).has_value()) {
            return EsmServer::ErrorResponse(req, status::conflict, "Object already exists, bucket: " + targetBucketErn + ", key: " + targetKey);
        }

        const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);

        Database::Entity::ESM::Object target = *source;
        target.oid.clear();// a new row, not an edit of the source's
        target.bucketErn = targetBucketErn;
        target.key = targetKey;
        target.ern = Core::createEsmObjectErn(auth.user->accountId, targetBucket->nameSpace, targetBucket->name + "/" + targetKey);
        target.created = std::chrono::system_clock::now();

        if (keepSource) {
            // Its own copy of the bytes, under its own internal name.
            target.internalName = Core::UuidUtils::CreateRandomUuid();
            std::error_code ec;
            std::filesystem::copy_file(std::filesystem::path(dataDir) / source->internalName,
                                       std::filesystem::path(dataDir) / target.internalName,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                log_error << "ESM could not copy object file, key: " << sourceKey << ", error: " << ec.message();
                return EsmServer::ErrorResponse(req, status::internal_server_error, "Could not copy object");
            }
        } else {
            // The move: the same file, answering to a different key from now on. The source row
            // goes away below, so nothing is left pointing at it.
            repo->deleteObjectByErn(source->ern);
        }

        const auto stored = repo->upsertObject(target);

        // Directory markers are not counted anywhere, so they must not move counters either.
        if (!Database::Entity::ESM::IsDirectoryKey(targetKey)) {
            if (auto bucket = repo->findBucketByErn(targetBucketErn); bucket.has_value()) {
                bucket->size += stored.size;
                bucket->objects++;
                repo->upsertBucket(*bucket);
            }
        }
        if (!keepSource && !Database::Entity::ESM::IsDirectoryKey(sourceKey)) {
            if (auto bucket = repo->findBucketByErn(sourceBucketErn); bucket.has_value()) {
                bucket->size = std::max<long>(0, bucket->size - stored.size);
                bucket->objects = std::max<long>(0, bucket->objects - 1);
                repo->upsertBucket(*bucket);
            }
        }

        // A move is a creation and a deletion, told in that order: a listener that keeps an index
        // of keys can apply them in the order it receives them and never have the object missing
        // from both places. A copy is only the creation.
        publishObjectEvent(kObjectCreated, stored, targetBucket, auth.user->userId);
        if (!keepSource) {
            publishObjectEvent(kObjectDeleted, *source, sourceBucket, auth.user->userId);
        }
        // Subscribers of the target bucket see an object appear, which is what happened as far as
        // anything watching that bucket is concerned - however it got there.
        notifyBucketSubscriptions(targetBucketErn, targetKey, stored.ern, stored.size, stored.contentType, stored.md5Sum);

        log_info << "ESM " << action << ", from: " << sourceBucketErn << "/" << sourceKey
                << ", to: " << targetBucketErn << "/" << targetKey << ", size: " << stored.size;

        return EsmServer::JsonResponse(req, status::ok, Dto::ESM::EsmMapper::toDto(stored).toJson());
    }

    static response<string_body> handleCopyObject(const request<string_body> &req) {

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CopyObjectRequest>(jv);
        return transferObject(req, "copy-object", request.sourceBucketErn, request.sourceKey,
                              request.targetBucketErn, request.targetKey, true);
    }

    static response<string_body> handleMoveObject(const request<string_body> &req) {

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CopyObjectRequest>(jv);
        return transferObject(req, "move-object", request.sourceBucketErn, request.sourceKey,
                              request.targetBucketErn, request.targetKey, false);
    }

    static response<string_body> handleRenameObject(const request<string_body> &req) {

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::RenameObjectRequest>(jv);
        // A move that cannot leave the bucket, which is the whole difference between the two.
        return transferObject(req, "rename-object", request.bucketErn, request.key,
                              request.bucketErn, request.newKey, false);
    }

    static response<string_body> handleAddObjectAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-object-attribute");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ObjectAttributeRequest>(jv);
        if (request.name.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        log_info << "ESM AddObjectAttribute, ern: " << request.ern << ", name: " << request.name;

        auto [object, bucket, error] = resolveObject(req, auth, request.ern);
        if (error.has_value()) return *error;

        if (object->attributes.contains(request.name)) {
            return EsmServer::ErrorResponse(req, status::conflict, "Attribute already exists, name: " + request.name);
        }

        object->attributes[request.name] = Dto::ESM::EsmMapper::toEntity(request.value);
        const auto stored = Database::RepositoryFactory::instance().esmRepository()->upsertObject(*object);
        // An attribute change is a change to the object, and anything watching it has no other
        // way to learn about it.
        publishObjectEvent(kObjectUpdated, stored, bucket, auth.user->userId);

        Dto::ESM::ObjectAttributeResponse response;
        response.ern = stored.ern;
        response.name = request.name;
        response.value = Dto::ESM::EsmMapper::toDto(stored.attributes.at(request.name));

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleSetObjectAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-object-attribute");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ObjectAttributeRequest>(jv);
        if (request.name.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        log_info << "ESM SetObjectAttribute, ern: " << request.ern << ", name: " << request.name;

        auto [object, bucket, error] = resolveObject(req, auth, request.ern);
        if (error.has_value()) return *error;

        if (!object->attributes.contains(request.name)) {
            return EsmServer::ErrorResponse(req, status::not_found, "Attribute not found, name: " + request.name);
        }

        object->attributes[request.name] = Dto::ESM::EsmMapper::toEntity(request.value);
        const auto stored = Database::RepositoryFactory::instance().esmRepository()->upsertObject(*object);
        // An attribute change is a change to the object, and anything watching it has no other
        // way to learn about it.
        publishObjectEvent(kObjectUpdated, stored, bucket, auth.user->userId);

        Dto::ESM::ObjectAttributeResponse response;
        response.ern = stored.ern;
        response.name = request.name;
        response.value = Dto::ESM::EsmMapper::toDto(stored.attributes.at(request.name));

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListObjectAttributes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-object-attributes");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ListObjectAttributesRequest>(jv);
        log_info << "ESM ListObjectAttributes, ern: " << request.ern;

        auto [object, bucket, error] = resolveObject(req, auth, request.ern);
        if (error.has_value()) return *error;

        Dto::ESM::ListObjectAttributesResponse response;
        response.ern = object->ern;
        for (const auto &[name, value]: object->attributes) {
            response.attributes[name] = Dto::ESM::EsmMapper::toDto(value);
        }
        response.total = static_cast<long>(response.attributes.size());

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteObjectAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-object-attribute");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::DeleteObjectAttributeRequest>(jv);
        if (request.name.empty()) {
            return EsmServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        log_info << "ESM DeleteObjectAttribute, ern: " << request.ern << ", name: " << request.name;

        auto [object, bucket, error] = resolveObject(req, auth, request.ern);
        if (error.has_value()) return *error;

        // Reported rather than ignored, for the same reason add and set are strict: a delete that
        // silently succeeds on a name nobody ever stored is the one way a typo could still go
        // unnoticed.
        if (!object->attributes.contains(request.name)) {
            return EsmServer::ErrorResponse(req, status::not_found, "Attribute not found, name: " + request.name);
        }

        object->attributes.erase(request.name);
        const auto stored = Database::RepositoryFactory::instance().esmRepository()->upsertObject(*object);
        // An attribute change is a change to the object, and anything watching it has no other
        // way to learn about it.
        publishObjectEvent(kObjectUpdated, stored, bucket, auth.user->userId);

        return EsmServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleAddBucketTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-bucket-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::ESM::AddBucketTagRequest>(jv);
        log_info << "ESM AddBucketTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        std::optional<Database::Entity::ESM::Bucket> bucket = repo->findBucketByErn(ern);
        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + ern);
        }
        bucket->tags[key] = value;
        bucket = repo->upsertBucket(bucket.value());

        return EsmServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleSetBucketTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-bucket-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::ESM::AddBucketTagRequest>(jv);
        log_info << "ESM SetBucketTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        std::optional<Database::Entity::ESM::Bucket> bucket = repo->findBucketByErn(ern);
        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + ern);
        }
        if (!bucket.value().tags.contains(key)) {
            return EsmServer::ErrorResponse(req, status::not_found, "Tag not found, key: " + key);
        }
        bucket->tags[key] = value;
        bucket = repo->upsertBucket(bucket.value());

        return EsmServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleDeleteBucketTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-bucket-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key] = boost::json::value_to<Dto::ESM::DeleteBucketTagRequest>(jv);
        log_info << "ESM DeleteBucketTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        std::optional<Database::Entity::ESM::Bucket> bucket = repo->findBucketByErn(ern);
        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + ern);
        }
        bucket->tags.erase(key);
        bucket = repo->upsertBucket(bucket.value());

        return EsmServer::JsonResponse(req, status::ok);
    }

    namespace {
        bool isEsmBucketErn(const std::string &ern) {
            return ern.starts_with("ern:esm:") && ern.find(":bucket:") != std::string::npos;
        }

        bool isEqsQueueErn(const std::string &ern) {
            return ern.starts_with("ern:eqs:") && ern.find(":queue:") != std::string::npos;
        }

        bool isEnsTopicErn(const std::string &ern) {
            return ern.starts_with("ern:ens:") && ern.find(":topic:") != std::string::npos;
        }
    }// namespace

    static response<string_body> handleSubscribe(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "subscribe");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::SubscribeRequest>(jv);
        log_info << "ESM Subscribe, sourceErn: " << request.sourceErn << ", type: " << request.type << ", targetErn: " << request.targetErn;

        if (request.type != "SQS" && request.type != "SNS") {
            return EsmServer::ErrorResponse(req, status::bad_request, "Unsupported subscription type (only SQS and SNS are supported for now): " + request.type);
        }
        if (!isEsmBucketErn(request.sourceErn)) {
            return EsmServer::ErrorResponse(req, status::bad_request, "sourceErn is not an ESM bucket ERN: " + request.sourceErn);
        }
        if (request.type == "SQS" && !isEqsQueueErn(request.targetErn)) {
            return EsmServer::ErrorResponse(req, status::bad_request, "targetErn is not an EQS queue ERN: " + request.targetErn);
        }
        if (request.type == "SNS" && !isEnsTopicErn(request.targetErn)) {
            return EsmServer::ErrorResponse(req, status::bad_request, "targetErn is not an ENS topic ERN: " + request.targetErn);
        }

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        if (!repo->findBucketByErn(request.sourceErn).has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.sourceErn);
        }
        if (request.type == "SQS" && !Database::RepositoryFactory::instance().eqsRepository()->findQueueByErn(request.targetErn).has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.targetErn);
        }
        if (request.type == "SNS" && !Database::RepositoryFactory::instance().ensRepository()->findTopicByErn(request.targetErn).has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + request.targetErn);
        }

        // Derived from the (sourceErn, type, targetErn) triple rather than randomly generated, so
        // that re-subscribing the same triple upserts the same document with a stable ERN instead
        // of silently reassigning its identity on every call (see ENS's identical subscribe).
        const auto subscriptionId = Core::CryptoUtils::md5Sum(request.sourceErn + ":" + request.type + ":" + request.targetErn);

        Database::Entity::ESM::Subscription subscription;
        subscription.accountId = auth.user->accountId;
        subscription.nameSpace = std::string(req["x-euclid-namespace"]);
        subscription.region = auth.user->region;
        subscription.owner = auth.user->userId;
        subscription.ern = Core::createErn("esm", auth.user->accountId, "subscription:" + subscriptionId);
        subscription.sourceErn = request.sourceErn;
        subscription.type = request.type;
        subscription.targetErn = request.targetErn;

        const auto saved = repo->upsertSubscription(subscription);

        Dto::ESM::SubscribeResponse response;
        response.ern = saved.ern;
        response.sourceErn = saved.sourceErn;
        response.type = saved.type;
        response.targetErn = saved.targetErn;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleUnsubscribe(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "unsubscribe");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ESM::UnsubscribeRequest::fromJson(req.body());
        log_info << "ESM Unsubscribe, ern: " << request.ern;

        Database::RepositoryFactory::instance().esmRepository()->deleteSubscriptionByErn(request.ern);

        return EsmServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleListSubscriptions(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-subscriptions");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ListSubscriptionsRequest>(jv);
        log_info << "ESM ListSubscriptions, bucketErn: " << request.bucketErn;

        const auto subscriptions = Database::RepositoryFactory::instance().esmRepository()->listSubscriptionsBySourceErn(request.bucketErn);

        Dto::ESM::ListSubscriptionsResponse response;
        response.subscriptions = Dto::ESM::EsmMapper::toDto(subscriptions);
        response.total = static_cast<long>(subscriptions.size());

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the ESM service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateBucket,
            DeleteBucket,
            RenameBucket,
            ListBuckets,
            GetBucketErn,
            GetBucketSize,
            AddBucketTag,
            SetBucketTag,
            DeleteBucketTag,
            PutObject,
            CreateUpload,
            UploadPart,
            CompleteUpload,
            GetObject,
            CreateDownload,
            DownloadPart,
            CompleteDownload,
            GetObjectCount,
            ListObjects,
            CopyObject,
            MoveObject,
            RenameObject,
            AddObjectAttribute,
            SetObjectAttribute,
            ListObjectAttributes,
            DeleteObjectAttribute,
            DeleteObject,
            PurgeBucket,
            Subscribe,
            Unsubscribe,
            ListSubscriptions,
            GetMetrics
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-bucket") return Command::CreateBucket;
        if (action == "delete-bucket") return Command::DeleteBucket;
        if (action == "list-buckets") return Command::ListBuckets;
        if (action == "get-bucket-ern") return Command::GetBucketErn;
        if (action == "get-bucket-size") return Command::GetBucketSize;
        if (action == "put-object") return Command::PutObject;
        if (action == "create-upload") return Command::CreateUpload;
        if (action == "upload-part") return Command::UploadPart;
        if (action == "complete-upload") return Command::CompleteUpload;
        if (action == "get-object") return Command::GetObject;
        if (action == "create-download") return Command::CreateDownload;
        if (action == "download-part") return Command::DownloadPart;
        if (action == "complete-download") return Command::CompleteDownload;
        if (action == "list-objects") return Command::ListObjects;
        if (action == "get-object-count") return Command::GetObjectCount;
        if (action == "copy-object") return Command::CopyObject;
        if (action == "move-object") return Command::MoveObject;
        if (action == "rename-object") return Command::RenameObject;
        if (action == "rename-bucket") return Command::RenameBucket;
        if (action == "add-object-attribute") return Command::AddObjectAttribute;
        if (action == "set-object-attribute") return Command::SetObjectAttribute;
        if (action == "list-object-attributes") return Command::ListObjectAttributes;
        if (action == "delete-object-attribute") return Command::DeleteObjectAttribute;
        if (action == "delete-object") return Command::DeleteObject;
        if (action == "purge-bucket") return Command::PurgeBucket;
        if (action == "add-bucket-tag") return Command::AddBucketTag;
        if (action == "set-bucket-tag") return Command::SetBucketTag;
        if (action == "delete-bucket-tag") return Command::DeleteBucketTag;
        if (action == "subscribe") return Command::Subscribe;
        if (action == "unsubscribe") return Command::Unsubscribe;
        if (action == "list-subscriptions") return Command::ListSubscriptions;
        if (action == "get-metrics") return Command::GetMetrics;
        return Command::Unknown;
    }

    // ── EsmServer ────────────────────────────────────────────────────────────

    EsmServer::EsmServer(std::string socketPath, const int threads) : HttpActionServer("ESM", std::move(socketPath), threads) {}

    EsmServer::~EsmServer() = default;

    response<string_body> EsmServer::Dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "ESM action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateBucket:
                return handleCreateBucket(req);

            case Command::DeleteBucket:
                return handleDeleteBucket(req);

            case Command::DeleteObject:
                return handleDeleteObject(req);

            case Command::ListBuckets:
                return handleListBuckets(req);

            case Command::GetBucketErn:
                return handleGetBucketErn(req);

            case Command::GetBucketSize:
                return handleGetBucketSize(req);

            case Command::PutObject:
                return handlePutObject(req);

            case Command::CreateUpload:
                return handleCreateUpload(req);

            case Command::UploadPart:
                return handleUploadPart(req);

            case Command::CompleteUpload:
                return handleCompleteUpload(req);

            case Command::GetObject:
                return handleGetObject(req);

            case Command::CreateDownload:
                return handleCreateDownload(req);

            case Command::DownloadPart:
                return handleDownloadPart(req);

            case Command::CompleteDownload:
                return handleCompleteDownload(req);

            case Command::ListObjects:
                return handleListObjects(req);

            case Command::GetObjectCount:
                return handleGetObjectCount(req);

            case Command::CopyObject:
                return handleCopyObject(req);

            case Command::MoveObject:
                return handleMoveObject(req);

            case Command::RenameBucket:
                return handleRenameBucket(req);

            case Command::RenameObject:
                return handleRenameObject(req);

            case Command::AddObjectAttribute:
                return handleAddObjectAttribute(req);

            case Command::SetObjectAttribute:
                return handleSetObjectAttribute(req);

            case Command::ListObjectAttributes:
                return handleListObjectAttributes(req);

            case Command::DeleteObjectAttribute:
                return handleDeleteObjectAttribute(req);

            case Command::PurgeBucket:
                return handlePurgeBucket(req);

            case Command::AddBucketTag:
                return handleAddBucketTag(req);

            case Command::SetBucketTag:
                return handleSetBucketTag(req);

            case Command::DeleteBucketTag:
                return handleDeleteBucketTag(req);

            case Command::Subscribe:
                return handleSubscribe(req);

            case Command::Unsubscribe:
                return handleUnsubscribe(req);

            case Command::ListSubscriptions:
                return handleListSubscriptions(req);

            case Command::Unknown:
            default:
                return ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

}// namespace Euclid::ESM