// Euclid includes
#include <StorageServer.h>

#include "euclid/dto/storage/DeleteObjectRequest.h"
#include "euclid/dto/storage/GetBucketSizeRequest.h"
#include "euclid/dto/storage/GetBucketSizeResponse.h"
#include "euclid/dto/storage/ListObjectsRequest.h"
#include "euclid/dto/storage/ListObjectsResponse.h"

namespace Euclid::Storage {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by SqsServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::Access::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        // Timer/counter names shared by every handler below - one series per action, labeled
        // "method"=<action>, e.g. name="queues-service-time" labelName="method" labelValue="send-message".
        constexpr auto kServiceTimer = "storage-service-time";
        constexpr auto kServiceCounter = "storage-service-count";

        // Fallback for "euclid.modules.storage.data-dir", matching the default in dist/etc/euclid*.json.
        constexpr auto kDefaultDataDir = "/usr/local/euclid/data/storage";

        // Name of the per-upload file recording the upload's target bucket/key, written by
        // create-upload and read back by upload-part/complete-upload. Its presence also marks a
        // storage under .../uploads/ as a valid, still-open upload.
        constexpr auto kUploadMetaFile = "upload.json";

        // Directory a multipart upload's parts are staged in until the upload is completed or aborted.
        std::filesystem::path uploadDirFor(const std::string &uploadId) {
            const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
            return std::filesystem::path(dataDir) / "uploads" / uploadId;
        }

        // Zero-padded so lexicographic storage-listing order matches numeric part order.
        std::string partFileName(const long partNumber) {
            std::ostringstream oss;
            oss << "part-" << std::setw(10) << std::setfill('0') << partNumber;
            return oss.str();
        }
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = StorageServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().accessRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return StorageServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // ── Action handlers ──────────────────────────────────────────────────────
    // Each handler parses whatever fields it needs out of the JSON request body.
    // Return a fully formed HTTP response.

    static response<string_body> handleCreateBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-bucket");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::CreateBucketRequest>(jv);

        Database::Entity::Storage::Bucket bucket;
        bucket.name = request.name;
        bucket.ern = Core::createStorageBucketErn(auth.user->accountId, request.name);
        bucket.region = auth.user->region;
        bucket.owner = auth.user->userId;

        const auto saved = Database::RepositoryFactory::instance().storageRepository()->upsertBucket(bucket);

        Dto::Storage::CreateBucketResponse response;
        response.name = saved.name;
        response.ern = saved.ern;

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListBuckets(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-buckets");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::ListBucketsRequest>(jv);
        log_info << "Storage ListBuckets" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto repo = Database::RepositoryFactory::instance().storageRepository();
        const std::vector<Database::Entity::Storage::Bucket> buckets = repo->listBuckets(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "Got bucket list, count: " << buckets.size();

        Dto::Storage::ListBucketsResponse response;
        response.buckets = Dto::Storage::StorageMapper::toDto(buckets);
        response.total = repo->countBuckets();

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetBucketErn(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-bucket-ern");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::GetBucketErnRequest>(jv);
        log_info << "ESS GetBucketErn, name: " << request.name;

        const std::optional<Database::Entity::Storage::Bucket> bucket = Database::RepositoryFactory::instance().storageRepository()->findBucketByName(request.name);
        log_debug << "Got ESS bucket ERN, name: " << request.name << ", ern: " << (bucket.has_value() ? bucket->ern : "(none)");

        if (!bucket.has_value()) {
            return StorageServer::ErrorResponse(req, status::not_found, "Bucket not found, name: " + request.name);
        }

        Dto::Storage::GetBucketErnResponse response;
        response.ern = bucket->ern;

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetBucketSize(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-bucket-size");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::GetBucketSizeRequest>(jv);
        log_info << "ESS GetBucketSize, ern: " << request.ern;

        const std::optional<Database::Entity::Storage::Bucket> bucket = Database::RepositoryFactory::instance().storageRepository()->findBucketByErn(request.ern);
        log_debug << "Got ESS bucket size, ern: " << request.ern << ", size: " << bucket->size;

        if (!bucket.has_value()) {
            return StorageServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.ern);
        }

        Dto::Storage::GetBucketSizeResponse response;
        response.ern = bucket->ern;
        response.size = bucket->size;

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-bucket");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::Storage::DeleteBucketRequest::fromJson(req.body());
        log_info << "Storage DeleteBucket, ern: " << request.ern;

        Database::RepositoryFactory::instance().storageRepository()->deleteBucketByErn(request.ern);

        return StorageServer::JsonResponse(req, status::ok);
    }

    // Starts a multipart upload: stages a scratch storage on disk that the "upload-part" action
    // will later copy each part into, keyed by an upload ID the caller carries for the lifetime of
    // the upload. Internal to the create-upload/upload-part/complete-upload workflow used by the
    // CLI/Java client, rather than a bucket-management action in its own right.
    static response<string_body> handleCreateUpload(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-upload");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::CreateUploadRequest>(jv);
        log_info << "Storage CreateUpload, bucketErn: " << request.bucketErn << ", key: " << request.key;

        if (const auto bucket = Database::RepositoryFactory::instance().storageRepository()->findBucketByErn(request.bucketErn); !bucket.has_value()) {
            return StorageServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.bucketErn);
        }

        const auto uploadId = Core::UuidUtils::CreateRandomUuid();
        const auto uploadDir = uploadDirFor(uploadId);

        std::error_code ec;
        std::filesystem::create_directories(uploadDir, ec);
        if (ec) {
            log_error << "Could not create upload storage, path: " << uploadDir.string() << ", error: " << ec.message();
            return StorageServer::ErrorResponse(req, status::internal_server_error, "Could not create upload storage");
        }

        // Records the upload's target bucket/key alongside the staged parts, so "complete-upload"
        // can assemble them into the right place using only the upload ID the client carries.
        const boost::json::value meta = {
                {"bucketErn", request.bucketErn},
                {"key", request.key},
                {"owner", auth.user->userId},
                {"created", Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())},
        };
        std::ofstream metaFile(uploadDir / kUploadMetaFile);
        metaFile << boost::json::serialize(meta);
        metaFile.close();

        log_info << "Created upload, id: " << uploadId << ", path: " << uploadDir.string();

        Dto::Storage::CreateUploadResponse response;
        response.uploadId = uploadId;
        response.bucketErn = request.bucketErn;
        response.key = request.key;

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
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
    static response<string_body> handleUploadPart(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "upload-part");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        const auto uploadId = std::string(req["x-euclid-upload-id"]);
        if (uploadId.empty()) {
            return StorageServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-upload-id header");
        }

        long partNumber = 0;
        try {
            partNumber = std::stol(std::string(req["x-euclid-part-number"]));
        } catch (const std::exception &) {
            return StorageServer::ErrorResponse(req, status::bad_request, "Missing or invalid x-euclid-part-number header");
        }

        const auto uploadDir = uploadDirFor(uploadId);
        if (!std::filesystem::exists(uploadDir / kUploadMetaFile)) {
            return StorageServer::ErrorResponse(req, status::not_found, "Upload not found, id: " + uploadId);
        }

        const auto &data = req.body();

        std::ofstream partFile(uploadDir / partFileName(partNumber), std::ios::binary | std::ios::trunc);
        if (!partFile.is_open()) {
            return StorageServer::ErrorResponse(req, status::internal_server_error, "Could not write upload part");
        }
        partFile.write(data.data(), static_cast<std::streamsize>(data.size()));
        partFile.close();

        log_debug << "Stored upload part, id: " << uploadId << ", part: " << partNumber << ", size: " << data.size();

        Dto::Storage::UploadPartResponse response;
        response.uploadId = uploadId;
        response.partNumber = partNumber;
        response.size = static_cast<long>(data.size());

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Assembles a completed multipart upload's staged parts into the final object under the
    // bucket's storage storage, then discards the upload's scratch storage. Internal to the
    // create-upload/upload-part/complete-upload workflow; called by the CLI's "upload-file" action
    // once all parts have been uploaded.
    static response<string_body> handleCompleteUpload(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "complete-upload");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::CompleteUploadRequest>(jv);
        log_info << "Storage CompleteUpload, id: " << request.uploadId;

        const auto uploadDir = uploadDirFor(request.uploadId);
        const auto metaPath = uploadDir / kUploadMetaFile;
        if (!std::filesystem::exists(metaPath)) {
            return StorageServer::ErrorResponse(req, status::not_found, "Upload not found, id: " + request.uploadId);
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

        const auto repo = Database::RepositoryFactory::instance().storageRepository();

        const auto bucket = repo->findBucketByErn(bucketErn);
        if (!bucket.has_value()) {
            return StorageServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + bucketErn);
        }

        // Zero-padded part filenames sort lexicographically in numeric order.
        std::vector<std::filesystem::path> parts;
        for (const auto &entry: std::filesystem::directory_iterator(uploadDir)) {
            if (entry.path().filename().string().starts_with("part-")) {
                parts.push_back(entry.path());
            }
        }
        std::ranges::sort(parts);

        if (parts.empty()) {
            return StorageServer::ErrorResponse(req, status::bad_request, "Upload has no parts, id: " + request.uploadId);
        }

        // Objects live in a flat storage named after a freshly generated UUID rather than
        // under a bucket/key path - the database (looked up below by bucketErn+key) is the only
        // place that mapping is recorded, so a bare storage listing can't be used to browse or
        // resolve objects by key.
        const auto existingObject = repo->findObjectByBucketAndKey(bucketErn, key);
        const auto internalName = Core::UuidUtils::CreateRandomUuid();

        const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
        const std::filesystem::path destPath = std::filesystem::path(dataDir) / internalName;

        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        if (ec) {
            log_error << "Could not create storage data storage, path: " << dataDir << ", error: " << ec.message();
            return StorageServer::ErrorResponse(req, status::internal_server_error, "Could not create storage data storage");
        }

        std::size_t totalSize = 0;
        {
            std::ofstream dest(destPath, std::ios::binary | std::ios::trunc);
            if (!dest.is_open()) {
                return StorageServer::ErrorResponse(req, status::internal_server_error, "Could not write object");
            }
            for (const auto &partPath: parts) {
                std::ifstream part(partPath, std::ios::binary);
                dest << part.rdbuf();
                totalSize += std::filesystem::file_size(partPath);
            }
        }

        std::filesystem::remove_all(uploadDir, ec);
        if (ec)
            log_warning << "Could not remove upload storage, path: " << uploadDir.string() << ", error: " << ec.message();

        Database::Entity::Storage::Object object;
        if (existingObject) object.oid = existingObject->oid;
        object.bucketErn = bucketErn;
        object.key = key;
        object.internalName = internalName;
        object.ern = Core::createStorageObjectErn(auth.user->accountId, bucket->name + "/" + key);
        object.owner = auth.user->userId;
        object.region = auth.user->region;
        object.size = static_cast<long>(totalSize);
        repo->upsertObject(object);

        // Update bucket
        Database::Entity::Storage::Bucket realBucket;
        realBucket.size += object.size;
        realBucket.objects++;
        realBucket = repo->upsertBucket(realBucket);
        log_debug << "Updated bucket, ern: " << realBucket.ern << ", size: " << realBucket.size << ", objects: " << realBucket.objects;

        // A re-upload to the same key replaces the DB row above; drop the now-unreferenced old file.
        if (existingObject && existingObject->internalName != internalName) {
            std::error_code oldEc;
            std::filesystem::remove(std::filesystem::path(dataDir) / existingObject->internalName, oldEc);
            if (oldEc)
                log_warning << "Could not remove superseded object file, internalName: " << existingObject->internalName << ", error: " << oldEc.message();
        }

        log_info << "Completed upload, id: " << request.uploadId << ", key: " << key << ", internalName: " << internalName << ", size: " << totalSize;

        Dto::Storage::CompleteUploadResponse response;
        response.bucketErn = bucketErn;
        response.key = key;
        response.ern = object.ern;
        response.size = static_cast<long>(totalSize);

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListObjects(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-objects");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::ListObjectsRequest>(jv);
        log_info << "ESS ListObjects, bucket: " << request.bucketErn << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto repo = Database::RepositoryFactory::instance().storageRepository();
        const std::vector<Database::Entity::Storage::Object> objects = repo->listObjects(request.bucketErn, request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "ESS got object list, bucket: " << request.bucketErn << ", count: " << objects.size();

        Dto::Storage::ListObjectsResponse response;
        response.objects = Dto::Storage::StorageMapper::toDto(objects);
        response.total = repo->countObjects();

        return StorageServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteObject(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-object");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::Storage::DeleteObjectRequest::fromJson(req.body());
        log_info << "Storage DeleteBucket, ern: " << request.ern;

        Database::RepositoryFactory::instance().storageRepository()->deleteObjectByErn(request.ern);

        return StorageServer::JsonResponse(req, status::ok);
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the SQS service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateBucket,
            DeleteBucket,
            ListBuckets,
            GetBucketErn,
            GetBucketSize,
            CreateUpload,
            UploadPart,
            CompleteUpload,
            ListObjects,
            DeleteObject,
            GetMetrics
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-bucket") return Command::CreateBucket;
        if (action == "delete-bucket") return Command::DeleteBucket;
        if (action == "list-buckets") return Command::ListBuckets;
        if (action == "get-bucket-ern") return Command::GetBucketErn;
        if (action == "get-bucket-size") return Command::GetBucketSize;
        if (action == "create-upload") return Command::CreateUpload;
        if (action == "upload-part") return Command::UploadPart;
        if (action == "complete-upload") return Command::CompleteUpload;
        if (action == "list-objects") return Command::ListObjects;
        if (action == "delete-object") return Command::DeleteObject;
        if (action == "get-metrics") return Command::GetMetrics;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return StorageServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EST action=" << action;

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

            case Command::CreateUpload:
                return handleCreateUpload(req);

            case Command::UploadPart:
                return handleUploadPart(req);

            case Command::CompleteUpload:
                return handleCompleteUpload(req);

            case Command::ListObjects:
                return handleListObjects(req);


            case Command::Unknown:
            default:
                return StorageServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── StorageServer ────────────────────────────────────────────────────────────

    StorageServer::StorageServer(std::string socketPath, const int threads) : HttpActionServer("storage", std::move(socketPath), threads) {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _lifecyleTaskId = scheduler.SchedulePeriodic("queues-reset-expired-messages", [] {
                                                         Database::RepositoryFactory::instance().sqsRepository()->resetExpiredMessages();
                                                     },
                                                     std::chrono::seconds(30));
    }

    StorageServer::~StorageServer() {
        Core::Scheduler::instance().Cancel(_lifecyleTaskId);
    }

    response<string_body> StorageServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::SQS