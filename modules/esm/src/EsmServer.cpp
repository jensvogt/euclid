// Euclid includes
#include <EsmServer.h>

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
        constexpr auto kServiceTimer = "storage-service-time";
        constexpr auto kServiceCounter = "storage-service-count";

        // Fallback for "euclid.modules.storage.data-dir", matching the default in dist/etc/euclid*.json.
#ifdef _WIN32
        constexpr auto kDefaultDataDir = "C:\\Program Files\\euclid\\data\\storage";
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

    // ── Action handlers ──────────────────────────────────────────────────────
    // Each handler parses whatever fields it needs out of the JSON request body.
    // Return a fully formed HTTP response.

    static response<string_body> handleCreateBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-bucket");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CreateBucketRequest>(jv);

        Database::Entity::ESM::Bucket bucket;
        bucket.name = request.name;
        bucket.ern = Core::createStorageBucketErn(auth.user->accountId, request.name);
        bucket.region = auth.user->region;
        bucket.owner = auth.user->userId;

        const auto saved = Database::RepositoryFactory::instance().esmRepository()->upsertBucket(bucket);
        log_info << "ESM bucket created, ern: " << bucket.ern;

        Dto::ESM::CreateBucketResponse response;
        response.name = saved.name;
        response.ern = saved.ern;

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListBuckets(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-buckets");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ListBucketsRequest>(jv);

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const std::vector<Database::Entity::ESM::Bucket> buckets = repo->listBuckets(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "ESM bucket list, count: " << buckets.size();

        Dto::ESM::ListBucketsResponse response;
        response.buckets = Dto::ESM::EsmMapper::toDto(buckets);
        response.total = repo->countBuckets();

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetBucketErn(const request<string_body> &req) {

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

    static response<string_body> handleGetBucketSize(const request<string_body> &req) {

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

    static response<string_body> handleDeleteBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-bucket");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ESM::DeleteBucketRequest::fromJson(req.body());
        log_info << "ESM bucket deleted, ern: " << request.ern;

        Database::RepositoryFactory::instance().esmRepository()->deleteBucketByErn(request.ern);

        return EsmServer::JsonResponse(req, status::ok);
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
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CreateUploadRequest>(jv);

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        if (!repo->findBucketByErn(request.bucketErn).has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + request.bucketErn);
        }

        // Seeds the object row with status CREATED right away, so its lifecycle is observable
        // from the very start of the upload rather than only appearing once complete-upload
        // finishes. upload-part/complete-upload advance status via the same bucketErn+key key.
        // Starts from any existing object at this key (a re-upload) rather than a blank one, so
        // its internalName/ern/size - and thus the still-valid previous file on disk - survive
        // until complete-upload actually replaces them.
        Database::Entity::ESM::Object object;
        if (const auto existing = repo->findObjectByBucketAndKey(request.bucketErn, request.key); existing.has_value()) {
            object = *existing;
        }
        object.bucketErn = request.bucketErn;
        object.key = request.key;
        object.owner = auth.user->userId;
        object.region = auth.user->region;
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
        const boost::json::value meta = {
                {"bucketErn", request.bucketErn},
                {"key", request.key},
                {"owner", auth.user->userId},
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
    static response<string_body> handleUploadPart(const request<string_body> &req) {

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
    static response<string_body> handleCompleteUpload(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "complete-upload");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::CompleteUploadRequest>(jv);
        log_info << "ESM CompleteUpload, id: " << request.uploadId;

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

        const auto repo = Database::RepositoryFactory::instance().esmRepository();

        const auto bucket = repo->findBucketByErn(bucketErn);
        if (!bucket.has_value()) {
            return EsmServer::ErrorResponse(req, status::not_found, "Bucket not found, ern: " + bucketErn);
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
            return EsmServer::ErrorResponse(req, status::bad_request, "Upload has no parts, id: " + request.uploadId);
        }

        // Objects live in a flat storage named after a freshly generated UUID rather than
        // under a bucket/key path - the database (looked up below by bucketErn+key) is the only
        // place that mapping is recorded, so a bare storage listing can't be used to browse or
        // resolve objects by key.
        const auto existingObject = repo->findObjectByBucketAndKey(bucketErn, key);
        const auto internalName = Core::UuidUtils::CreateRandomUuid();
        const auto ern = Core::createStorageObjectErn(auth.user->accountId, bucket->name + "/" + key);

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

        // Detached rather than joined: Dispatch() must return promptly so the gateway worker
        // thread handling this request isn't tied up for as long as a multi-GB file takes to
        // assemble and hash. Everything it touches is captured by value (paths, strings, the
        // existingObject snapshot) since req and the variables above go out of scope once this
        // handler returns. The whole body is wrapped in try/catch: an exception escaping a
        // detached thread's entry function calls std::terminate() and takes down the entire
        // process, unlike an exception in a normal request handler which route()/Dispatch() would
        // otherwise catch.
        std::thread([repo, uploadDir, parts, dataDir, destPath, internalName, ern, bucketErn, key, owner, region, existingObject, uploadId = request.uploadId] {
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
                object.size = static_cast<long>(assembledSize);
                object.status = Database::Entity::ESM::ObjectStatus::COMPLETED;
                object.contentType = contentType;
                object.md5Sum = md5Sum;
                repo->upsertObject(object);

                // Re-fetches the bucket rather than reusing the snapshot from before assembly
                // started - post-processing can take a while for a large file, so that snapshot
                // may be stale by now. Still starts from a real bucket (not a default-constructed
                // one): upsertBucket() keys on ern, and upserting a blank one would create/
                // accumulate into a separate phantom bucket instead of updating this one.
                if (auto freshBucket = repo->findBucketByErn(bucketErn); freshBucket.has_value()) {
                    freshBucket->size += object.size;
                    freshBucket->objects++;
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

    static response<string_body> handleListObjects(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-objects");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ESM::ListObjectsRequest>(jv);
        log_info << "ESM ListObjects, bucket: " << request.bucketErn << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto repo = Database::RepositoryFactory::instance().esmRepository();
        const std::vector<Database::Entity::ESM::Object> objects = repo->listObjects(request.bucketErn, request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "ESM got object list, bucket: " << request.bucketErn << ", count: " << objects.size();

        Dto::ESM::ListObjectsResponse response;
        response.objects = Dto::ESM::EsmMapper::toDto(objects);
        response.total = repo->countObjects(request.bucketErn);

        return EsmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteObject(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-object");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EsmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ESM::DeleteObjectRequest::fromJson(req.body());
        log_info << "Storage DeleteObject, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().esmRepository();

        // Looked up before deleting so the bucket's aggregate size/objects can be adjusted -
        // without this, a bucket's stats would only ever grow, never reflecting deletions.
        if (const auto object = repo->findObjectByErn(request.ern); object.has_value()) {
            if (auto bucket = repo->findBucketByErn(object->bucketErn); bucket.has_value()) {
                bucket->size = std::max<long>(0, bucket->size - object->size);
                bucket->objects = std::max<long>(0, bucket->objects - 1);
                repo->upsertBucket(*bucket);
            }

            const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.storage.data-dir", kDefaultDataDir);
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(dataDir) / object->internalName, ec);
            if (ec)
                log_warning << "Could not remove object file, internalName: " << object->internalName << ", error: " << ec.message();
        }

        repo->deleteObjectByErn(request.ern);

        return EsmServer::JsonResponse(req, status::ok);
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
            return EsmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
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
                return EsmServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EsmServer ────────────────────────────────────────────────────────────

    EsmServer::EsmServer(std::string socketPath, const int threads) : HttpActionServer("storage", std::move(socketPath), threads) {
    }

    EsmServer::~EsmServer() = default;

    response<string_body> EsmServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::ESM