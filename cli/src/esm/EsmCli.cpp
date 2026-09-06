// Euclid includes
#include <euclid/cli/esm/EsmCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Serializes stderr writes from uploadPart() when it's called concurrently from
        // uploadFile()'s worker futures, so two failing parts can't interleave their error lines.
        std::mutex &CerrMutex() {
            static std::mutex mutex;
            return mutex;
        }

        // Retry tuning for the whole create-upload/upload-part/complete-upload sequence. Parts are
        // the hot path (thousands of calls for a large file), so they're the ones most likely to
        // hit a transient failure - e.g. a request landing on a storage instance the gateway's
        // autoscaler is mid-way through killing. A handful of quick retries turns that into a brief
        // stall instead of aborting the whole upload. create-upload/complete-upload - and their
        // download counterparts - are called once each, but bracket every part: giving up on the
        // first transient 5xx there throws away the entire file, so they retry on the same terms.
        constexpr int kMaxPartAttempts = 4;
        constexpr std::chrono::milliseconds kPartRetryBaseDelay{500};

        // upload-file/download-file's --part-size/--concurrency defaults, overridable per
        // deployment via euclid.modules.esm.part-size/concurrency in the loaded configuration
        // file (see main.cpp's --config) instead of only ever falling back to the DEFAULT_PART_SIZE/
        // DEFAULT_CONCURRENCY compiled-in constants.
        long DefaultPartSize() {
            return Core::Configuration::instance().getOr<long>("euclid.modules.esm.part-size", static_cast<long>(DEFAULT_PART_SIZE));
        }

        int DefaultConcurrency() {
            return Core::Configuration::instance().getOr<int>("euclid.modules.esm.concurrency", DEFAULT_CONCURRENCY);
        }

        // Joins root with an object key's prefix-stripped remainder to get downloadBucket()'s
        // local destination path. Object keys are opaque strings, not real filesystem paths - a
        // key starting with '/' (e.g. someone having uploaded files keyed by their original
        // absolute local path) doesn't mean "OS-absolute", so it's normalized to relative rather
        // than rejected; std::filesystem::path::operator/ would otherwise silently discard root
        // and resolve to that absolute path directly, which relative_path() here prevents. ".."
        // components are still refused, though - a key is server-returned data, not necessarily
        // benign local input, so walking back out of root entirely is the one thing this guards
        // against, the same class of guard as unpacking an untrusted archive (zip-slip).
        // std::nullopt means "refuse it".
        std::optional<std::filesystem::path> SafeJoin(const std::filesystem::path &root, const std::string &remainder) {
            std::filesystem::path rel(remainder);
            if (rel.is_absolute()) rel = rel.relative_path();
            for (const auto &part: rel) {
                if (part == "..") return std::nullopt;
            }
            return root / rel;
        }

    }

    EsmCli::EsmCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EsmCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("esm", {
                                           {"add-bucket-tag", "Adds a tag to a bucket"},
                                           {"add-object-attribute", "Adds an attribute to an object"},
                                           {"copy-object", "Copies an object to another key or bucket"},
                                           {"create-bucket", "Create a new bucket"},
                                           {"delete-objects", "Delete several objects from a bucket, by key or by prefix"},
                                           {"set-bucket-internal", "Hide a bucket from listings, or stop hiding it"},
                                           {"delete-bucket", "Delete a bucket"},
                                           {"delete-bucket-tag", "Deletes a tag from a bucket"},
                                           {"delete-object", "Deletes an object by ERN"},
                                           {"delete-object-attribute", "Deletes an attribute from an object"},
                                           {"disable-encryption", "Stop encrypting the objects written to a bucket from now on"},
                                           {"download-file", "Download an object from a bucket to a local file"},
                                           {"download-bucket", "Download a bucket's objects to a local directory"},
                                           {"enable-encryption", "Encrypt the objects written to a bucket from now on"},
                                           {"get-bucket-ern", "Resolve a bucket's ERN by name"},
                                           {"get-bucket-size", "Returns the bucket size in bytes"},
                                           {"get-object-count", "Return the number of objects in a bucket"},
                                           {"list-buckets", "List buckets"},
                                           {"list-objects", "List objects"},
                                           {"list-object-attributes", "Lists the attributes of an object"},
                                           {"list-subscriptions", "Lists the subscriptions of a bucket"},
                                           {"move-object", "Moves an object to another key or bucket"},
                                           {"purge-bucket", "Removes all objects from a bucket"},
                                           {"rename-bucket", "Give a bucket another name"},
                                           {"touch-object", "Re-send notifications for objects already in a bucket"},
                                           {"rename-object", "Renames an object within its bucket"},
                                           {"set-bucket-tag", "Sets the value of an existing bucket tag"},
                                           {"set-object-attribute", "Sets the value of an existing object attribute"},
                                           {"subscribe", "Subscribes a target resource (an EQS queue or an ENS topic) to a bucket's object-created events"},
                                           {"unsubscribe", "Deletes a subscription"},
                                           {"upload-file", "Upload a local file to a bucket"},
                                           {"upload-directory", "Upload every file in a local directory to a bucket"},
                                   });
        }
        if (action == "delete-objects") {
            return deleteObjects(args);
        }
        if (action == "set-bucket-internal") {
            return setBucketInternal(args);
        }
        if (action == "create-bucket") {
            return createBucket(args);
        }
        if (action == "delete-bucket") {
            return deleteBucket(args);
        }
        if (action == "touch-object") {
            return touchObject(args);
        }
        if (action == "rename-bucket") {
            return renameBucket(args);
        }
        if (action == "enable-encryption") {
            return enableEncryption(args);
        }
        if (action == "disable-encryption") {
            return disableEncryption(args);
        }
        if (action == "list-buckets") {
            return listBuckets(args);
        }
        if (action == "get-bucket-ern") {
            return getBucketErn(args);
        }
        if (action == "get-bucket-size") {
            return getBucketSize(args);
        }
        if (action == "upload-file") {
            return uploadFile(args);
        }
        if (action == "upload-directory") {
            return uploadDirectory(args);
        }
        if (action == "download-file") {
            return downloadFile(args);
        }
        if (action == "download-bucket") {
            return downloadBucket(args);
        }
        if (action == "list-objects") {
            return listObjects(args);
        }
        if (action == "get-object-count") {
            return getObjectCount(args);
        }
        if (action == "delete-object") {
            return deleteObject(args);
        }
        if (action == "purge-bucket") {
            return purgeBucket(args);
        }
        if (action == "add-bucket-tag") {
            return addBucketTag(args);
        }
        if (action == "set-bucket-tag") {
            return setBucketTag(args);
        }
        if (action == "delete-bucket-tag") {
            return deleteBucketTag(args);
        }
        if (action == "copy-object") {
            return copyObject(args);
        }
        if (action == "move-object") {
            return moveObject(args);
        }
        if (action == "rename-object") {
            return renameObject(args);
        }
        if (action == "add-object-attribute") {
            return addObjectAttribute(args);
        }
        if (action == "set-object-attribute") {
            return setObjectAttribute(args);
        }
        if (action == "list-object-attributes") {
            return listObjectAttributes(args);
        }
        if (action == "delete-object-attribute") {
            return deleteObjectAttribute(args);
        }
        if (action == "subscribe") {
            return subscribe(args);
        }
        if (action == "unsubscribe") {
            return unsubscribe(args);
        }
        if (action == "list-subscriptions") {
            return listSubscriptions(args);
        }
        std::cerr << "error: unknown ESM action '" << action << "'\n";
        return 1;
    }

    int EsmCli::createBucket(const std::vector<std::string> &args) const {
        po::options_description desc("create bucket options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name")
                ("internal,i", po::bool_switch(), "euclid's own plumbing: create it, but leave it out of list-buckets and the bucket count");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "create-bucket", "--name <name> [--internal]",
                                   "Creates a new storage bucket with the given name. "
                                   "--internal marks it as euclid's own plumbing rather than somebody's bucket: it "
                                   "works in every way an ordinary bucket does, but is left out of list-buckets and "
                                   "the bucket count, so it does not clutter a listing for people who have no reason "
                                   "to act on it. Hidden, not protected - anyone who knows the name can still use it. "
                                   "Use \"esm set-bucket-internal\" to change this afterwards.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::CreateBucketRequest request;
        request.name = vm["name"].as<std::string>();
        request.internal = vm["internal"].as<bool>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "create-bucket", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: create-bucket failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::deleteBucket(const std::vector<std::string> &args) const {
        po::options_description desc("delete bucket options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace")
                ("async", po::bool_switch()->default_value(false), "return at once and remove the objects in the background; for buckets too large to empty within one request");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "delete-bucket", "--bucket <name|ern> [--async]",
                                   "Deletes a storage bucket identified by its Euclid resource name (ERN), "
                                   "along with every object it contains - their stored files are removed and one "
                                   "\"esm.object.deleted\" event is published per object, exactly as if each had been "
                                   "deleted on its own, so anything listening for objects in the bucket learns what "
                                   "went. Use \"esm purge-bucket\" to empty a bucket while keeping it, optionally "
                                   "restricted to a key prefix. "
                                   "Give --async for a bucket large enough that emptying it takes minutes: the request is answered at once "
                                   "with HTTP 202, and a background thread inside ESM removes the objects and then the bucket - which stays "
                                   "listed, and still deletable, until it is genuinely gone. Without it the call sits there until the "
                                   "gateway times out while the removal carries on unseen behind it. This cannot be undone.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::DeleteBucketRequest request;
        request.ern = vm["bucket"].as<std::string>();

        auto body = boost::json::value_from(request);
        if (vm["async"].as<bool>()) body.as_object()["async"] = true;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "delete-bucket", body); !response.IsSuccess()) {
                std::cerr << "error: delete-bucket failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::listBuckets(const std::vector<std::string> &args) const {
        po::options_description desc("list buckets options");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "bucket name prefix")
                ("page-size,s", po::value<long>()->default_value(-1), "page size")
                ("page-index,i", po::value<long>()->default_value(-1), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "list-buckets", "[--prefix <prefix>] [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists storage buckets, optionally filtered by name prefix and paginated. Paginated: page-size defaults to 10, page-index to 0,"
                                   " sort-column to \"created\" and sort-direction to \"asc\".",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::ListBucketsRequest request;
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "list-buckets", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-buckets failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::getBucketErn(const std::vector<std::string> &args) const {
        po::options_description desc("get bucket ern options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "get-bucket-ern", "--name <name>",
                                   "Resolves the Euclid resource name (ERN) of a bucket by its name.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::GetBucketErnRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "get-bucket-ern", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-bucket-ern failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::getBucketSize(const std::vector<std::string> &args) const {
        po::options_description desc("get bucket size options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "get-bucket-size", "--bucket <name|ern>",
                                   "Returns the bucket size in bytes.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::GetBucketSizeRequest request;
        request.ern = vm["bucket"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "get-bucket-size", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-bucket-size failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    std::optional<boost::json::value> EsmCli::putObject(const std::string &bucketErn, const std::string &key, const std::string &data) const {
        const std::vector<std::pair<std::string, std::string> > headers{
                {"x-euclid-bucket-ern", bucketErn},
                {"x-euclid-key", key},
        };

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.PostBinary("esm", "put-object", headers, data);
            if (!response.IsSuccess()) {
                std::cerr << "error: put-object failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return std::nullopt;
            }
            return response.body;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return std::nullopt;
        }
    }

    std::optional<std::string> EsmCli::createUpload(const std::string &bucketErn, const std::string &key, const int concurrency) const {
        Dto::ESM::CreateUploadRequest request;
        request.bucketErn = bucketErn;
        request.key = key;

        // Declares the concurrency the upload is about to use so the gateway's autoscaler can
        // ramp storage instances toward it directly instead of waiting to sample busy instances -
        // see ServiceController::declareExpectedConcurrency()'s doc comment for why that reactive
        // sampling alone falls short for this workload (many short-lived part uploads).
        const std::vector<std::pair<std::string, std::string> > headers{
                {"x-euclid-expected-concurrency", std::to_string(concurrency)},
        };

        // Retried on 5xx exactly like uploadPart() below. Safe to repeat: the object row the server
        // seeds is upserted on bucketErn+key, so a second attempt updates the same row rather than
        // adding another one - the only cost of a retry is the scratch directory the abandoned
        // upload ID left behind.
        for (int attempt = 1; attempt <= kMaxPartAttempts; ++attempt) {
            const bool lastAttempt = attempt == kMaxPartAttempts;

            try {
                const HttpClient client(_endpoint, _authentication, _caCertPath);
                const HttpResponse response = client.Post("esm", "create-upload", boost::json::value_from(request), headers);
                if (response.IsSuccess()) {
                    return boost::json::value_to<Dto::ESM::CreateUploadResponse>(response.body).uploadId;
                }

                if (response.statusCode < 500 || lastAttempt) {
                    std::cerr << "error: create-upload failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return std::nullopt;
                }
                std::cerr << "warning: create-upload failed (attempt " << attempt << "/" << kMaxPartAttempts << ", HTTP " << response.statusCode << "), retrying..." << std::endl;
            } catch (const std::exception &ex) {
                if (lastAttempt) {
                    std::cerr << "error: " << ex.what() << std::endl;
                    return std::nullopt;
                }
                std::cerr << "warning: create-upload failed (attempt " << attempt << "/" << kMaxPartAttempts << "): " << ex.what() << ", retrying..." << std::endl;
            }

            std::this_thread::sleep_for(kPartRetryBaseDelay * attempt);
        }
        return std::nullopt;// unreachable
    }

    // upload-part does NOT conform to the JSON in/out convention every other action uses here -
    // uploadId/partNumber ride as headers and data goes as a raw "application/octet-stream" body
    // instead of a base64 JSON field, to speed up transfer of what's typically the bulk of an
    // upload's bytes. It's internal-only (uploadFile() is the only caller), so there's no external
    // client relying on a stable JSON schema for it.
    bool EsmCli::uploadPart(const std::string &uploadId, const long partNumber, const std::string &data) const {
        const std::vector<std::pair<std::string, std::string> > headers{
                {"x-euclid-upload-id", uploadId},
                {"x-euclid-part-number", std::to_string(partNumber)},
        };

        for (int attempt = 1; attempt <= kMaxPartAttempts; ++attempt) {
            const bool lastAttempt = attempt == kMaxPartAttempts;

            try {
                const HttpClient client(_endpoint, _authentication, _caCertPath);
                const HttpResponse response = client.PostBinary("esm", "upload-part", headers, data);
                if (response.IsSuccess()) return true;

                // A 4xx means the request itself is wrong (bad upload ID, malformed part, etc.) -
                // retrying won't change that. 5xx/network failures are the transient kind retries
                // are for.
                const bool retryable = response.statusCode >= 500;
                const std::lock_guard lock(CerrMutex());
                if (!retryable || lastAttempt) {
                    std::cerr << "error: upload-part failed for part " << partNumber << " (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return false;
                }
                std::cerr << "warning: upload-part failed for part " << partNumber << " (attempt " << attempt << "/" << kMaxPartAttempts << ", HTTP " << response.statusCode << "), retrying..." << std::endl;
            } catch (const std::exception &ex) {
                const std::lock_guard lock(CerrMutex());
                if (lastAttempt) {
                    std::cerr << "error: upload-part failed for part " << partNumber << " after " << kMaxPartAttempts << " attempts: " << ex.what() << std::endl;
                    return false;
                }
                std::cerr << "warning: upload-part failed for part " << partNumber << " (attempt " << attempt << "/" << kMaxPartAttempts << "): " << ex.what() << ", retrying..." << std::endl;
            }

            std::this_thread::sleep_for(kPartRetryBaseDelay * attempt);
        }
        return false;// unreachable
    }

    std::optional<boost::json::value> EsmCli::completeUpload(const std::string &uploadId) const {
        Dto::ESM::CompleteUploadRequest request;
        request.uploadId = uploadId;

        // Retried on 5xx like createUpload() above, and for the same reason: failing here discards
        // every part already uploaded. Safe to repeat as long as the request is rejected before the
        // server takes ownership of the staged parts - it validates and hands assembly to a
        // background pass, so a 5xx from that validation means nothing was consumed. An upload the
        // server did accept fails a retry with 404 (upload not found), which is not retried.
        for (int attempt = 1; attempt <= kMaxPartAttempts; ++attempt) {
            const bool lastAttempt = attempt == kMaxPartAttempts;

            try {
                const HttpClient client(_endpoint, _authentication, _caCertPath);
                const HttpResponse response = client.Post("esm", "complete-upload", boost::json::value_from(request));
                if (response.IsSuccess()) return response.body;

                if (response.statusCode < 500 || lastAttempt) {
                    std::cerr << "error: complete-upload failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return std::nullopt;
                }
                std::cerr << "warning: complete-upload failed (attempt " << attempt << "/" << kMaxPartAttempts << ", HTTP " << response.statusCode << "), retrying..." << std::endl;
            } catch (const std::exception &ex) {
                if (lastAttempt) {
                    std::cerr << "error: " << ex.what() << std::endl;
                    return std::nullopt;
                }
                std::cerr << "warning: complete-upload failed (attempt " << attempt << "/" << kMaxPartAttempts << "): " << ex.what() << ", retrying..." << std::endl;
            }

            std::this_thread::sleep_for(kPartRetryBaseDelay * attempt);
        }
        return std::nullopt;// unreachable
    }

    BinaryHttpResponse EsmCli::getObject(const std::string &bucketErn, const std::string &key, const long maxInlineSize) const {
        const std::vector<std::pair<std::string, std::string> > headers{
                {"x-euclid-bucket-ern", bucketErn},
                {"x-euclid-key", key},
                {"x-euclid-part-size", std::to_string(maxInlineSize)},
        };

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            return client.PostForBinary("esm", "get-object", headers);
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return {.statusCode = 0};
        }
    }

    std::optional<Dto::ESM::CreateDownloadResponse> EsmCli::createDownload(const std::string &bucketErn, const std::string &key, const int concurrency) const {
        Dto::ESM::CreateDownloadRequest request;
        request.bucketErn = bucketErn;
        request.key = key;

        // Declares the concurrency the download is about to use so the gateway's autoscaler can
        // ramp storage instances toward it directly instead of waiting to sample busy instances -
        // see createUpload()'s identical comment. Without this, downloadFile()'s burst of part
        // requests arrives faster than reactive sampling can react to, so the autoscaler ends up
        // thrashing (spinning up an instance, letting it go idle, tearing it down, repeat) instead
        // of holding a steady pool sized for the declared concurrency.
        const std::vector<std::pair<std::string, std::string> > headers{
                {"x-euclid-expected-concurrency", std::to_string(concurrency)},
        };

        // Retried on 5xx exactly like createUpload() and downloadPart(). Safe to repeat: the
        // download session it opens is server-side scratch state keyed by a fresh download ID, so a
        // retried attempt starts a new one and the abandoned session is simply never used.
        for (int attempt = 1; attempt <= kMaxPartAttempts; ++attempt) {
            const bool lastAttempt = attempt == kMaxPartAttempts;

            try {
                const HttpClient client(_endpoint, _authentication, _caCertPath);
                const HttpResponse response = client.Post("esm", "create-download", boost::json::value_from(request), headers);
                if (response.IsSuccess()) {
                    return boost::json::value_to<Dto::ESM::CreateDownloadResponse>(response.body);
                }

                if (response.statusCode < 500 || lastAttempt) {
                    std::cerr << "error: create-download failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return std::nullopt;
                }
                std::cerr << "warning: create-download failed (attempt " << attempt << "/" << kMaxPartAttempts << ", HTTP " << response.statusCode << "), retrying..." << std::endl;
            } catch (const std::exception &ex) {
                if (lastAttempt) {
                    std::cerr << "error: " << ex.what() << std::endl;
                    return std::nullopt;
                }
                std::cerr << "warning: create-download failed (attempt " << attempt << "/" << kMaxPartAttempts << "): " << ex.what() << ", retrying..." << std::endl;
            }

            std::this_thread::sleep_for(kPartRetryBaseDelay * attempt);
        }
        return std::nullopt;// unreachable
    }

    // download-part does NOT conform to the JSON in/out convention every other action uses here -
    // downloadId/partNumber/partSize ride as headers, and the response body is raw
    // "application/octet-stream" bytes instead of JSON (see HttpClient::PostForBinary()). It's
    // internal-only (downloadFile() is the only caller), so there's no external client relying on
    // a stable JSON schema for it.
    bool EsmCli::downloadPart(const std::string &downloadId, const long partNumber, const long partSize, const std::string &filePath) const {
        const std::vector<std::pair<std::string, std::string> > headers{
                {"x-euclid-download-id", downloadId},
                {"x-euclid-part-number", std::to_string(partNumber)},
                {"x-euclid-part-size", std::to_string(partSize)},
        };

        for (int attempt = 1; attempt <= kMaxPartAttempts; ++attempt) {
            const bool lastAttempt = attempt == kMaxPartAttempts;

            try {
                const HttpClient client(_endpoint, _authentication, _caCertPath);
                const BinaryHttpResponse response = client.PostForBinary("esm", "download-part", headers);
                if (response.IsSuccess()) {
                    // A separate file handle per part rather than one shared across all worker
                    // futures: parts land in this function concurrently (up to <concurrency> at a
                    // time), and each only ever writes its own non-overlapping byte range, so
                    // independent handles avoid contending on a shared std::fstream's single file
                    // position for no benefit.
                    std::fstream out(filePath, std::ios::binary | std::ios::in | std::ios::out);
                    if (!out.is_open()) {
                        const std::lock_guard lock(CerrMutex());
                        std::cerr << "error: could not open file '" << filePath << "' to write part " << partNumber << "\n";
                        return false;
                    }
                    out.seekp((partNumber - 1) * partSize);
                    out.write(response.data.data(), static_cast<std::streamsize>(response.data.size()));
                    return true;
                }

                // A 4xx means the request itself is wrong (bad download ID, malformed part, etc.) -
                // retrying won't change that. 5xx/network failures are the transient kind retries
                // are for.
                const bool retryable = response.statusCode >= 500;
                const std::lock_guard lock(CerrMutex());
                if (!retryable || lastAttempt) {
                    std::cerr << "error: download-part failed for part " << partNumber << " (HTTP " << response.statusCode << "): " << boost::json::serialize(response.errorBody) << std::endl;
                    return false;
                }
                std::cerr << "warning: download-part failed for part " << partNumber << " (attempt " << attempt << "/" << kMaxPartAttempts << ", HTTP " << response.statusCode << "), retrying..." << std::endl;
            } catch (const std::exception &ex) {
                const std::lock_guard lock(CerrMutex());
                if (lastAttempt) {
                    std::cerr << "error: download-part failed for part " << partNumber << " after " << kMaxPartAttempts << " attempts: " << ex.what() << std::endl;
                    return false;
                }
                std::cerr << "warning: download-part failed for part " << partNumber << " (attempt " << attempt << "/" << kMaxPartAttempts << "): " << ex.what() << ", retrying..." << std::endl;
            }

            std::this_thread::sleep_for(kPartRetryBaseDelay * attempt);
        }
        return false;// unreachable
    }

    bool EsmCli::completeDownload(const std::string &downloadId) const {
        Dto::ESM::CompleteDownloadRequest request;
        request.downloadId = downloadId;

        // Retried on 5xx like completeUpload(), and for the same reason: failing here throws away
        // every part already downloaded. Safe to repeat - it only releases the session's server-side
        // scratch state, and a session already released fails a retry with 404, which is not retried.
        for (int attempt = 1; attempt <= kMaxPartAttempts; ++attempt) {
            const bool lastAttempt = attempt == kMaxPartAttempts;

            try {
                const HttpClient client(_endpoint, _authentication, _caCertPath);
                const HttpResponse response = client.Post("esm", "complete-download", boost::json::value_from(request));
                if (response.IsSuccess()) return true;

                if (response.statusCode < 500 || lastAttempt) {
                    std::cerr << "error: complete-download failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return false;
                }
                std::cerr << "warning: complete-download failed (attempt " << attempt << "/" << kMaxPartAttempts << ", HTTP " << response.statusCode << "), retrying..." << std::endl;
            } catch (const std::exception &ex) {
                if (lastAttempt) {
                    std::cerr << "error: " << ex.what() << std::endl;
                    return false;
                }
                std::cerr << "warning: complete-download failed (attempt " << attempt << "/" << kMaxPartAttempts << "): " << ex.what() << ", retrying..." << std::endl;
            }

            std::this_thread::sleep_for(kPartRetryBaseDelay * attempt);
        }
        return false;// unreachable
    }

    int EsmCli::uploadOneFile(const std::string &bucketErn, const std::string &key, const std::string &filePath, long partSize, int concurrency, boost::json::value &outResult) const {

        // A caller that has no opinion about how the file is cut up says so by passing nothing,
        // and gets the same defaults "esm upload-file" would have applied - the configured ones,
        // not whatever constant happened to be nearest.
        if (partSize <= 0) partSize = DefaultPartSize();
        if (concurrency <= 0) concurrency = DefaultConcurrency();

        std::error_code fsEc;
        const auto fileSize = std::filesystem::file_size(filePath, fsEc);
        if (fsEc) {
            std::cerr << "error: could not stat file '" << filePath << "': " << fsEc.message() << std::endl;
            return 1;
        }

        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "error: could not open file '" << filePath << "'\n";
            return 1;
        }

        // Objects that fit under a single part skip multipart entirely: put-object stores them in
        // one request/response round trip instead of paying for create-upload + one upload-part +
        // complete-upload, none of which buys anything when there's only ever going to be one part.
        if (fileSize < static_cast<std::uintmax_t>(partSize)) {
            std::string data(static_cast<std::size_t>(fileSize), '\0');
            if (fileSize > 0) {
                in.read(data.data(), static_cast<std::streamsize>(fileSize));
                data.resize(static_cast<std::size_t>(in.gcount()));
            }
            const auto result = putObject(bucketErn, key, data);
            if (!result) return 1;
            outResult = *result;
            return 0;
        }

        const auto uploadId = createUpload(bucketErn, key, concurrency);
        if (!uploadId) return 1;

        // Bounded pipeline: parts are read from disk sequentially (cheap, and std::ifstream isn't
        // safe to read concurrently) but their uploads run in the background, up to <concurrency>
        // at a time, so network latency for one part overlaps with reading/uploading the next.
        bool ok = true;
        std::vector<std::future<bool> > inFlight;

        // Reaps ANY finished future, not just the oldest submission: a plain FIFO wait-on-front
        // means one slow-for-any-reason part (retry backoff, a scheduling hiccup, whatever) blocks
        // the entire pipeline from submitting new work even while every other slot has long since
        // finished - observed in practice as long flat stretches with zero progress followed by a
        // burst once the stuck one finally resolves. Polling for whichever slot finishes first
        // keeps all <concurrency> slots genuinely busy instead of head-of-line-blocked on one.
        const auto reapOneCompleted = [&]() -> bool {
            for (auto it = inFlight.begin(); it != inFlight.end(); ++it) {
                if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    if (!it->get()) ok = false;
                    inFlight.erase(it);
                    return true;
                }
            }
            return false;
        };

        const auto waitForSlot = [&] {
            while (!reapOneCompleted()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };

        const auto submitPart = [&](const long partNumber, std::string data) {
            if (static_cast<int>(inFlight.size()) >= concurrency) waitForSlot();
            inFlight.push_back(std::async(std::launch::async, [this, id = *uploadId, partNumber, data = std::move(data)] {
                return uploadPart(id, partNumber, data);
            }));
        };

        if (fileSize == 0) {
            submitPart(1, {});
        } else {
            std::vector<char> buffer(static_cast<std::size_t>(partSize));
            long partNumber = 1;
            while (ok && (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)) {
                submitPart(partNumber, std::string(buffer.data(), static_cast<std::size_t>(in.gcount())));
                ++partNumber;
            }
        }

        while (!inFlight.empty()) waitForSlot();
        if (!ok) return 1;

        const auto result = completeUpload(*uploadId);
        if (!result) return 1;

        outResult = *result;
        return 0;
    }

    int EsmCli::uploadFile(const std::vector<std::string> &args) const {
        po::options_description desc("upload file options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN of the target bucket")
                ("key,k", po::value<std::string>()->required(), "destination key (path) within the bucket")
                ("file,f", po::value<std::string>()->required(), "local file to upload")
                ("part-size,s", po::value<long>()->default_value(DefaultPartSize()), "part size in bytes")
                ("concurrency,j", po::value<int>()->default_value(DefaultConcurrency()), "number of parts to upload in parallel");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "upload-file", "--bucket <name|ern> --key <key> --file <path> [--part-size <bytes>] [--concurrency <n>]",
                                   "Uploads a local file to a bucket. Files smaller than --part-size are stored in a single request; "
                                   "larger files are transparently split into parts for a multipart upload, uploading up to <concurrency> "
                                   "parts at a time in background threads.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        const auto bucketErn = vm["bucket"].as<std::string>();
        const auto key = vm["key"].as<std::string>();
        const auto filePath = vm["file"].as<std::string>();
        const auto partSize = vm["part-size"].as<long>();
        const auto concurrency = std::max(1, vm["concurrency"].as<int>());

        boost::json::value result;
        if (uploadOneFile(bucketErn, key, filePath, partSize, concurrency, result) != 0) return 1;

        Core::WriteJson(std::cout, result, _pretty);
        return 0;
    }

    int EsmCli::uploadDirectory(const std::vector<std::string> &args) const {
        po::options_description desc("upload directory options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN of the target bucket")
                ("dir,d", po::value<std::string>()->required(), "local directory to upload")
                ("prefix,p", po::value<std::string>()->default_value(""), "prefix prepended to each object's key")
                ("recursive,r", po::bool_switch()->default_value(false), "recurse into subdirectories")
                ("part-size,s", po::value<long>()->default_value(DefaultPartSize()), "part size in bytes")
                ("concurrency,j", po::value<int>()->default_value(DefaultConcurrency()), "number of parts to upload in parallel, per file");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "upload-directory", "--bucket <name|ern> --dir <path> [--prefix <prefix>] [--recursive] [--part-size <bytes>] [--concurrency <n>]",
                                   "Uploads every file in a local directory to a bucket, one object per file. Each object's key is "
                                   "--prefix followed by the file's path relative to --dir (forward slashes regardless of platform). "
                                   "Without --recursive, only files directly inside --dir are uploaded; with it, subdirectories are "
                                   "walked too. Each file uses the same put-object/multipart logic as upload-file, chosen by its own "
                                   "size relative to --part-size.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        const auto bucketErn = vm["bucket"].as<std::string>();
        const auto dirPath = vm["dir"].as<std::string>();
        const auto prefix = vm["prefix"].as<std::string>();
        const bool recursive = vm["recursive"].as<bool>();
        const auto partSize = vm["part-size"].as<long>();
        const auto concurrency = std::max(1, vm["concurrency"].as<int>());

        std::error_code fsEc;
        const std::filesystem::path root(dirPath);
        if (!std::filesystem::is_directory(root, fsEc) || fsEc) {
            std::cerr << "error: '" << dirPath << "' is not a directory\n";
            return 1;
        }

        // Walked up front into a sorted list rather than uploaded while iterating, so the
        // directory_iterator is closed (and any transient walk error caught) before the first
        // upload starts, and files are processed in a deterministic order regardless of what
        // order the filesystem happens to hand them back in.
        std::vector<std::filesystem::path> files;
        std::error_code walkEc;
        constexpr auto walkOpts = std::filesystem::directory_options::skip_permission_denied;
        if (recursive) {
            for (auto it = std::filesystem::recursive_directory_iterator(root, walkOpts, walkEc);
                 !walkEc && it != std::filesystem::recursive_directory_iterator(); it.increment(walkEc)) {
                if (it->is_regular_file(walkEc)) files.push_back(it->path());
            }
        } else {
            for (auto it = std::filesystem::directory_iterator(root, walkOpts, walkEc);
                 !walkEc && it != std::filesystem::directory_iterator(); it.increment(walkEc)) {
                if (it->is_regular_file(walkEc)) files.push_back(it->path());
            }
        }
        if (walkEc) {
            std::cerr << "error: could not walk directory '" << dirPath << "': " << walkEc.message() << "\n";
            return 1;
        }
        std::ranges::sort(files);

        if (files.empty()) {
            std::cerr << "warning: no files found under '" << dirPath << "'" << (recursive ? "" : " (use --recursive to include subdirectories)") << "\n";
        }

        bool ok = true;
        boost::json::array results;
        for (const auto &file: files) {
            const auto relative = std::filesystem::relative(file, root, fsEc);
            const std::string key = prefix + (fsEc ? file.filename().string() : relative.generic_string());

            boost::json::value uploadResult;
            if (uploadOneFile(bucketErn, key, file.string(), partSize, concurrency, uploadResult) != 0) {
                std::cerr << "error: failed to upload '" << file.string() << "' to key '" << key << "'\n";
                ok = false;
                results.push_back(boost::json::object{{"key", key}, {"file", file.string()}, {"error", true}});
                continue;
            }
            results.push_back(boost::json::object{{"key", key}, {"file", file.string()}, {"result", uploadResult}});
        }

        Core::WriteJson(std::cout, results, _pretty);
        return ok ? 0 : 1;
    }

    int EsmCli::downloadOneFile(const std::string &bucketErn, const std::string &key, const std::string &filePath, const long partSize, const int concurrency, boost::json::value &outResult) const {
        if (const auto parent = std::filesystem::path(filePath).parent_path(); !parent.empty()) {
            std::error_code mkEc;
            std::filesystem::create_directories(parent, mkEc);
            if (mkEc) {
                std::cerr << "error: could not create directory '" << parent.string() << "': " << mkEc.message() << "\n";
                return 1;
            }
        }

        // Objects that fit under a single part skip multipart entirely: get-object fetches them in
        // one request/response round trip instead of paying for create-download + one
        // download-part + complete-download. Unlike uploadOneFile() (which stats the local file
        // upfront and so already knows before making any request whether it's small), a download's
        // size isn't known until asked, so this is tried unconditionally first; HTTP 413 means the
        // object turned out to be too large, and the multipart path below takes over instead.
        constexpr int kPayloadTooLarge = 413;
        if (const auto inlineResponse = getObject(bucketErn, key, partSize); inlineResponse.statusCode != kPayloadTooLarge) {
            if (inlineResponse.statusCode == 0) return 1;// network failure, already printed
            if (!inlineResponse.IsSuccess()) {
                std::cerr << "error: get-object failed (HTTP " << inlineResponse.statusCode << "): " << boost::json::serialize(inlineResponse.errorBody) << std::endl;
                return 1;
            }

            std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                std::cerr << "error: could not open file '" << filePath << "' for writing\n";
                return 1;
            }
            out.write(inlineResponse.data.data(), static_cast<std::streamsize>(inlineResponse.data.size()));
            out.close();

            outResult = boost::json::value{
                    {"bucketErn", bucketErn},
                    {"key", key},
                    {"size", static_cast<long>(inlineResponse.data.size())},
                    {"file", filePath},
            };
            return 0;
        }

        const auto created = createDownload(bucketErn, key, concurrency);
        if (!created) return 1;

        const auto &downloadId = created->downloadId;
        const auto totalSize = created->size;

        // Pre-sizes the destination file so worker threads below can each write their assigned
        // byte range directly at the right offset regardless of completion order - the download
        // equivalent of uploadOneFile() reading its source file sequentially while letting uploads
        // themselves complete out of order.
        {
            std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                std::cerr << "error: could not open file '" << filePath << "' for writing\n";
                return 1;
            }
            if (totalSize > 0) {
                out.seekp(totalSize - 1);
                out.put('\0');
            }
        }

        // Bounded pipeline, same shape as uploadOneFile()'s: up to <concurrency> part downloads run
        // in the background at a time, and whichever slot finishes first is reaped so one
        // slow-for-any-reason part can't head-of-line-block the rest (see uploadOneFile()'s
        // reapOneCompleted() for why that matters in practice).
        bool ok = true;
        std::vector<std::future<bool> > inFlight;

        const auto reapOneCompleted = [&]() -> bool {
            for (auto it = inFlight.begin(); it != inFlight.end(); ++it) {
                if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    if (!it->get()) ok = false;
                    inFlight.erase(it);
                    return true;
                }
            }
            return false;
        };

        const auto waitForSlot = [&] {
            while (!reapOneCompleted()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };

        const auto submitPart = [&](const long partNumber) {
            if (static_cast<int>(inFlight.size()) >= concurrency) waitForSlot();
            inFlight.push_back(std::async(std::launch::async, [this, downloadId, partNumber, partSize, filePath] {
                return downloadPart(downloadId, partNumber, partSize, filePath);
            }));
        };

        for (long offset = 0, partNumber = 1; offset < totalSize; offset += partSize, ++partNumber) {
            submitPart(partNumber);
        }

        while (!inFlight.empty()) waitForSlot();
        if (!ok) return 1;

        if (!completeDownload(downloadId)) return 1;

        outResult = boost::json::value{
                {"bucketErn", created->bucketErn},
                {"key", created->key},
                {"ern", created->ern},
                {"size", created->size},
                {"contentType", created->contentType},
                {"file", filePath},
        };
        return 0;
    }

    int EsmCli::downloadFile(const std::vector<std::string> &args) const {
        po::options_description desc("download file options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN of the source bucket")
                ("key,k", po::value<std::string>()->required(), "key (path) of the object within the bucket")
                ("file,f", po::value<std::string>()->required(), "local destination file")
                ("part-size,s", po::value<long>()->default_value(DefaultPartSize()), "part size in bytes")
                ("concurrency,j", po::value<int>()->default_value(DefaultConcurrency()), "number of parts to download in parallel");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "download-file", "--bucket <name|ern> --key <key> --file <path> [--part-size <bytes>] [--concurrency <n>]",
                                   "Downloads an object from a bucket to a local file. Objects smaller than --part-size are fetched in a "
                                   "single request; larger objects are transparently split into parts for a multipart download, downloading "
                                   "up to <concurrency> parts at a time in background threads.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        const auto bucketErn = vm["bucket"].as<std::string>();
        const auto key = vm["key"].as<std::string>();
        const auto filePath = vm["file"].as<std::string>();
        const auto partSize = vm["part-size"].as<long>();
        const auto concurrency = std::max(1, vm["concurrency"].as<int>());

        boost::json::value result;
        if (downloadOneFile(bucketErn, key, filePath, partSize, concurrency, result) != 0) return 1;

        Core::WriteJson(std::cout, result, _pretty);
        return 0;
    }

    std::optional<std::vector<Dto::ESM::Object> > EsmCli::listAllObjects(const std::string &bucketErn, const std::string &prefix) const {
        std::vector<Dto::ESM::Object> objects;

        for (long pageIndex = 0;; ++pageIndex) {
            constexpr long kPageSize = 1000;
            Dto::ESM::ListObjectsRequest request;
            request.bucketErn = bucketErn;
            request.prefix = prefix;
            request.pageSize = kPageSize;
            request.pageIndex = pageIndex;
            request.sortColumn = "key";

            try {
                const HttpClient client(_endpoint, _authentication, _caCertPath);
                const HttpResponse response = client.Post("esm", "list-objects", boost::json::value_from(request));
                if (!response.IsSuccess()) {
                    std::cerr << "error: list-objects failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return std::nullopt;
                }
                const auto page = boost::json::value_to<Dto::ESM::ListObjectsResponse>(response.body);
                objects.insert(objects.end(), page.objects.begin(), page.objects.end());
                if (static_cast<long>(page.objects.size()) < kPageSize) break;
            } catch (const std::exception &ex) {
                std::cerr << "error: " << ex.what() << std::endl;
                return std::nullopt;
            }
        }
        return objects;
    }

    int EsmCli::downloadBucket(const std::vector<std::string> &args) const {
        po::options_description desc("download bucket options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN of the source bucket")
                ("dir,d", po::value<std::string>()->required(), "local directory to download into")
                ("prefix,p", po::value<std::string>()->default_value(""), "object key prefix filter")
                ("recursive,r", po::bool_switch()->default_value(false), "recurse through all matching keys, not just direct descendants")
                ("part-size,s", po::value<long>()->default_value(DefaultPartSize()), "part size in bytes")
                ("concurrency,j", po::value<int>()->default_value(DefaultConcurrency()), "number of parts to download in parallel, per file")
                ("zip,z", po::value<std::string>(), "also pack --dir's contents into a ZIP archive at this path once downloaded");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "download-bucket", "--bucket <name|ern> --dir <path> [--prefix <prefix>] [--recursive] [--part-size <bytes>] [--concurrency <n>] [--zip <path>]",
                                   "Downloads objects from a bucket to a local directory, one file per object. Without --recursive, only "
                                   "an object's direct descendants are downloaded - keys matching --prefix with no further '/' after it; "
                                   "with --recursive, every key matching --prefix is downloaded, however deeply nested. Each local file's "
                                   "path is --dir followed by the object's key with --prefix stripped off the front. Each object uses the "
                                   "same get-object/multipart logic as download-file, chosen by its own size relative to --part-size. If "
                                   "--zip is given, --dir's contents are additionally packed into a ZIP archive at that path once every "
                                   "object has been downloaded; the files downloaded under --dir are left in place.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        const auto bucketErn = vm["bucket"].as<std::string>();
        const auto dirPath = vm["dir"].as<std::string>();
        const auto prefix = vm["prefix"].as<std::string>();
        const bool recursive = vm["recursive"].as<bool>();
        const auto partSize = vm["part-size"].as<long>();
        const auto concurrency = std::max(1, vm["concurrency"].as<int>());
        const bool wantZip = vm.contains("zip");
        const auto zipPath = wantZip ? vm["zip"].as<std::string>() : std::string();

        const auto objects = listAllObjects(bucketErn, prefix);
        if (!objects) return 1;

        // Direct descendants only (no --recursive): a key's remainder after stripping --prefix
        // must have no further '/' - the same "delimiter" semantics S3-style listings use to
        // simulate a directory's immediate contents, since objects don't actually have real
        // parent directories.
        std::vector<const Dto::ESM::Object *> matched;
        for (const auto &object: *objects) {
            if (const std::string remainder = object.key.size() >= prefix.size() ? object.key.substr(prefix.size()) : object.key; !recursive && remainder.find('/') != std::string::npos) continue;
            matched.push_back(&object);
        }

        if (matched.empty()) {
            std::cerr << "warning: no matching objects under prefix '" << prefix << "'" << (recursive ? "" : " (use --recursive to include nested keys)") << "\n";
        }

        const std::filesystem::path root(dirPath);
        std::error_code mkRootEc;
        std::filesystem::create_directories(root, mkRootEc);
        if (mkRootEc) {
            std::cerr << "error: could not create directory '" << dirPath << "': " << mkRootEc.message() << "\n";
            return 1;
        }

        bool ok = true;
        boost::json::array results;
        for (const auto *object: matched) {
            const std::string remainder = object->key.size() >= prefix.size() ? object->key.substr(prefix.size()) : object->key;
            const auto filePath = SafeJoin(root, remainder);
            if (!filePath) {
                std::cerr << "error: refusing key '" << object->key << "' - its path would escape '" << dirPath << "'\n";
                ok = false;
                results.push_back(boost::json::object{{"key", object->key}, {"error", true}});
                continue;
            }

            boost::json::value downloadResult;
            if (downloadOneFile(bucketErn, object->key, filePath->string(), partSize, concurrency, downloadResult) != 0) {
                std::cerr << "error: failed to download key '" << object->key << "' to '" << filePath->string() << "'\n";
                ok = false;
                results.push_back(boost::json::object{{"key", object->key}, {"file", filePath->string()}, {"error", true}});
                continue;
            }
            results.push_back(boost::json::object{{"key", object->key}, {"file", filePath->string()}, {"result", downloadResult}});
        }

        if (wantZip) {
            try {
                Core::ZipUtils::Zip(dirPath, zipPath);
            } catch (const std::exception &ex) {
                std::cerr << "error: could not create zip file '" << zipPath << "': " << ex.what() << "\n";
                ok = false;
            }

            boost::json::object out;
            out["objects"] = results;
            out["zip"] = zipPath;
            Core::WriteJson(std::cout, out, _pretty);
        } else {
            Core::WriteJson(std::cout, results, _pretty);
        }
        return ok ? 0 : 1;
    }

    int EsmCli::listObjects(const std::vector<std::string> &args) const {
        po::options_description desc("list objects options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace")
                ("prefix,p", po::value<std::string>(), "object name prefix")
                ("page-size,s", po::value<long>()->default_value(-1), "page size")
                ("page-index,i", po::value<long>()->default_value(-1), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "list-objects", "--bucket <name|ern> [--prefix <prefix>] [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists storage objects by bucket, optionally filtered by name prefix and paginated. Paginated: page-size defaults to 10, page-index to 0,"
                                   " sort-column to \"name\" and sort-direction to \"asc\".",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::ListObjectsRequest request;
        request.bucketErn = vm["bucket"].as<std::string>();
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        request.sortDirection = vm["sort-direction"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "list-objects", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-objects failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::getObjectCount(const std::vector<std::string> &args) const {
        po::options_description desc("get object count options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace")
                ("prefix,p", po::value<std::string>(), "object key prefix");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "get-object-count", "--bucket <name|ern>",
                                   "Returns the number of objects in a bucket, optionally filtered by object key prefix and paginated. The return object contains the "
                                   "ERN and the number of objects.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::GetObjectCountRequest request;
        request.ern = vm["bucket"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }
        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "get-object-count", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-object-count failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::deleteObject(const std::vector<std::string> &args) const {
        po::options_description desc("delete object options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "euclid resource name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "delete-object", "--ern <ern>",
                                   "Deletes a storage object identified by its Euclid resource name (ERN).",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::DeleteObjectRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "delete-object", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-object failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::purgeBucket(const std::vector<std::string> &args) const {
        po::options_description desc("purge bucket options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace")
                ("prefix,p", po::value<std::string>(), "object key prefix")
                ("async", po::bool_switch()->default_value(false), "return at once and remove the objects in the background; for buckets too large to empty within one request");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "purge-bucket", "--bucket <name|ern> [--prefix <value>] [--async]",
                                   "Removes all objects from a bucket identified by its Euclid resource name (ERN), leaving the (empty) "
                                   "bucket itself in place, optionally filtered by object key prefix. It returns the ERN and the number of remaining objects. "
                                   "Give --async for a bucket large enough that emptying it takes minutes: the request is answered at once "
                                   "with HTTP 202 and the object count at the time of asking, and the objects are removed by a background "
                                   "thread inside ESM, instead of the call sitting there until the gateway times out while the removal "
                                   "carries on unseen behind it. Watch the progress with \"esm get-bucket-size\".",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::PurgeBucketRequest request;
        request.ern = vm["bucket"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        // Added to the serialized request rather than carried on the DTO: "async" says how the
        // caller wants to be answered, not what it wants done, and every other field here
        // describes the latter.
        auto body = boost::json::value_from(request);
        if (vm["async"].as<bool>()) body.as_object()["async"] = true;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "purge-bucket", body);
            if (!response.IsSuccess()) {
                std::cerr << "error: purge-bucket failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::addBucketTag(const std::vector<std::string> &args) const {
        po::options_description desc("add bucket tag options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace")
                ("key,k", po::value<std::string>()->required(), "tag key")
                ("value,v", po::value<std::string>()->required(), "tag value");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "add-bucket-tag", "--bucket <name|ern> --key <value> --value <value>",
                                   "Adds a tag to a bucket. If the bucket tag exists already the tag value will be set, otherwise "
                                   "the value will be updated.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::AddBucketTagRequest request;
        request.ern = vm["bucket"].as<std::string>();
        request.key = vm["key"].as<std::string>();
        request.value = vm["value"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "add-bucket-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: add-bucket-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::setBucketTag(const std::vector<std::string> &args) const {
        po::options_description desc("set bucket tag options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace")
                ("key,k", po::value<std::string>()->required(), "tag key")
                ("value,v", po::value<std::string>()->required(), "tag value");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "set-bucket-tag", "--bucket <name|ern> --key <value> --value <value>",
                                   "Sets a value for an existing bucket tag. The bucket tag must be existing already.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::SetBucketTagRequest request;
        request.ern = vm["bucket"].as<std::string>();
        request.key = vm["key"].as<std::string>();
        request.value = vm["value"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "set-bucket-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: set-bucket-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::deleteBucketTag(const std::vector<std::string> &args) const {
        po::options_description desc("delete bucket tag options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace")
                ("key,k", po::value<std::string>()->required(), "tag key");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "delete-bucket-tag", "--bucket <name|ern> --key <value>",
                                   "Deletes a tag from a bucket.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::DeleteBucketTagRequest request;
        request.ern = vm["bucket"].as<std::string>();
        request.key = vm["key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "delete-bucket-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-bucket-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    // copy-object and move-object take the same arguments and differ only in what becomes of the
    // source, so they are one implementation with a flag - the same way the module handles them.
    int EsmCli::transferObject(const std::vector<std::string> &args, const bool keepSource) const {
        const std::string action = keepSource ? "copy-object" : "move-object";

        po::options_description desc(keepSource ? "copy object options" : "move object options");
        desc.add_options()
                ("source-bucket,b", po::value<std::string>()->required(), "ERN of the bucket the object is in")
                ("source-key,k", po::value<std::string>()->required(), "key of the object within that bucket")
                ("target-bucket,B", po::value<std::string>(), "ERN of the bucket it should end up in; defaults to the source bucket")
                ("target-key,K", po::value<std::string>()->required(), "key it should have there");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", action,
                                   "--source-bucket <ern> --source-key <key> --target-key <key> [--target-bucket <ern>]",
                                   keepSource
                                       ? "Copies an object to another key, in the same bucket or a different one. The copy gets its own "
                                       "bytes and its own lifetime, so deleting either object leaves the other intact, and it starts with "
                                       "the source's content type, checksum and attributes. Refuses rather than overwriting if something "
                                       "is already stored at the target key."
                                       : "Moves an object to another key, in the same bucket or a different one. Nothing is copied - an "
                                       "object's bytes are addressed internally, so a move is a change of key however large the object is. "
                                       "Refuses rather than overwriting if something is already stored at the target key.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        const auto sourceBucket = vm["source-bucket"].as<std::string>();
        Dto::ESM::CopyObjectRequest request;
        request.sourceBucketErn = sourceBucket;
        request.sourceKey = vm["source-key"].as<std::string>();
        request.targetBucketErn = vm.contains("target-bucket") ? vm["target-bucket"].as<std::string>() : sourceBucket;
        request.targetKey = vm["target-key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", action, boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::copyObject(const std::vector<std::string> &args) const {
        return transferObject(args, true);
    }

    int EsmCli::moveObject(const std::vector<std::string> &args) const {
        return transferObject(args, false);
    }

    int EsmCli::deleteObjects(const std::vector<std::string> &args) const {
        po::options_description desc("delete objects options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket to delete from; a name is enough, a full ERN also works")
                ("keys,k", po::value<std::string>(), "comma-separated object keys to delete")
                ("prefix,p", po::value<std::string>(), "delete everything whose key starts with this; omit both to delete every object in the bucket")
                ("async", po::bool_switch()->default_value(false), "return at once and delete in the background; for lists or prefixes too large to remove within one request");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "delete-objects",
                                   "--bucket <name|ern> [--keys <list>] [--prefix <key-prefix>] [--async]",
                                   "Deletes objects from one bucket: the ones named by --keys, or everything under "
                                   "--prefix. Each object's file, its row and one delete event go, exactly as a "
                                   "single-object delete would do it, so subscribers see the same thing either way. "
                                   "This is the middle case between \"delete-object\", which takes one object and is "
                                   "what an SDK calls per object, and \"purge-bucket\", which is about emptying a "
                                   "bucket rather than removing things from it. "
                                   "--keys and --prefix ask different questions and cannot be combined: naming both "
                                   "is refused rather than guessed at, since the mistake is not recoverable. Naming "
                                   "neither deletes every object in the bucket, which is what \"purge-bucket\" does "
                                   "and worth being sure about. "
                                   "A key that names nothing is skipped rather than failing the batch - a list "
                                   "assembled earlier should not fail because one object went in the meantime, and the "
                                   "answer reports how many were asked for and how many actually went. "
                                   "Give --async for a list or prefix large enough that removing it takes minutes: the "
                                   "request is answered at once with HTTP 202, and the deleting is done by a "
                                   "background thread inside ESM instead of the call sitting there until the gateway "
                                   "times out while the removal carries on unseen behind it.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        if (vm.contains("keys") && vm.contains("prefix")) {
            std::cerr << "error: delete-objects failed: name either --keys or --prefix, not both\n";
            return 1;
        }

        boost::json::object request{
                {"ern", vm["bucket"].as<std::string>()},
                {"async", vm["async"].as<bool>()}};

        if (vm.contains("keys")) {
            boost::json::array keys;
            std::stringstream ss(vm["keys"].as<std::string>());
            for (std::string part; std::getline(ss, part, ',');) {
                const auto first = part.find_first_not_of(" \t");
                const auto last = part.find_last_not_of(" \t");
                if (first == std::string::npos) continue;
                keys.push_back(boost::json::string(part.substr(first, last - first + 1)));
            }
            if (keys.empty()) {
                std::cerr << "error: delete-objects failed: --keys named no key\n";
                return 1;
            }
            request["keys"] = keys;
        }
        if (vm.contains("prefix")) request["prefix"] = vm["prefix"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "delete-objects", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-objects failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::touchObject(const std::vector<std::string> &args) const {
        po::options_description desc("touch object options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket holding the objects; a name is enough, a full ERN also works")
                ("prefix,p", po::value<std::string>()->default_value(""), "only objects whose key starts with this; omit for every object in the bucket")
                ("async", po::bool_switch()->default_value(false), "return at once and announce the objects in the background; for buckets too large to announce within one request");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "touch-object", "--bucket <name|ern> [--prefix <key-prefix>] [--async]",
                                   "Announces objects that are already in a bucket, as though each had just been "
                                   "uploaded: the same esm.object.created event and the same bucket subscription "
                                   "deliveries an upload would have produced. "
                                   "Nothing about the objects changes - not a byte of them, and not their timestamps "
                                   "either. \"Touch\" here means what it does to listeners, not what it does to "
                                   "storage; a modified time is something consumers compare against, and moving it "
                                   "would make this destructive in exactly the way it is meant not to be. "
                                   "It exists because a notification can be lost in ways an object never is - a "
                                   "consumer that was down while its subscription was live rather than durable, a "
                                   "queue deleted with deliveries still queued for it, or a subscription created "
                                   "after the objects had already arrived. In all of those the data is intact and "
                                   "only the announcement was lost, and re-uploading gigabytes to re-send a few "
                                   "kilobytes of notification is the wrong repair. "
                                   "The prefix is matched against the whole key, so it selects one object when it is "
                                   "a full key and a directory when it ends in a slash. Omitting it announces every "
                                   "object in the bucket, which on a large one is a great many notifications - the "
                                   "answer says how many were sent. "
                                   "Directory markers are left out, since an upload never announces those either. "
                                   "Note that this is a replay, not a repair of one: every subscriber of the bucket "
                                   "hears about every selected object, including consumers that processed it the "
                                   "first time. Only run it where the consumers are idempotent - one that is not "
                                   "will do its work twice, and on a whole bucket it will do it twice for "
                                   "everything. Narrowing the prefix to what was actually missed is usually safer "
                                   "than touching the bucket. "
                                   "Give --async for a bucket with enough objects that announcing them takes minutes: "
                                   "the request is answered at once with HTTP 202 and the object count at the time of "
                                   "asking, and the notifications are sent by a background thread inside ESM, instead "
                                   "of the call sitting there until the gateway times out while the announcing carries "
                                   "on unseen behind it. Nothing is resumable either way - a run that is interrupted "
                                   "has simply announced fewer objects, and asking again announces all of them.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        const boost::json::object request{
                {"ern", vm["bucket"].as<std::string>()},
                {"prefix", vm["prefix"].as<std::string>()},
                {"async", vm["async"].as<bool>()}};

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "touch-object", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: touch-object failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::setBucketInternal(const std::vector<std::string> &args) const {
        po::options_description desc("set bucket internal options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket to change; a name is enough, a full ERN also works")
                ("internal,i", po::value<bool>()->default_value(true), "true to hide it from listings, false to show it again");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "set-bucket-internal", "--bucket <name|ern> [--internal true|false]",
                                   "Marks a bucket as euclid's own plumbing, or stops doing so. An internal bucket "
                                   "works in every way an ordinary one does - objects are written, read, copied, "
                                   "moved and deleted the same - but it is left out of list-buckets and the bucket "
                                   "count, so it does not clutter a listing for people who have no reason to act on "
                                   "it. "
                                   "The bucket applications are deployed from is what this exists for: its contents "
                                   "are artifacts EAP puts there and replaces on a redeploy, and nobody browsing "
                                   "their own buckets needs to step around it. "
                                   "Hidden, not protected. Anyone who knows the name can still read and write the "
                                   "bucket, which is exactly what the component that created it does - so this is a "
                                   "tidiness measure, not an access control. Use \"eam\" grants for that. "
                                   "Reversible: --internal false puts it back in the listing exactly as it was.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        const boost::json::object request{
                {"ern", vm["bucket"].as<std::string>()},
                {"internal", vm["internal"].as<bool>()}};

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "set-bucket-internal", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: set-bucket-internal failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::renameBucket(const std::vector<std::string> &args) const {
        po::options_description desc("rename bucket options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "ERN of the bucket to rename")
                ("new-name,n", po::value<std::string>()->required(), "name it should have");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "rename-bucket", "--bucket <name|ern> --new-name <name>",
                                   "Gives a bucket another name. Nothing is copied and no object moves: what changes is the name, and "
                                   "with it everything that spells it out - the bucket's own ERN, every object's reference to it, every "
                                   "object's ERN (which carries the bucket name before the key) and every subscription watching it. "
                                   "The answer says how many of each were rewritten. "
                                   "Refused if the new name is taken, or if a transfer server is serving the bucket: that server would "
                                   "keep pointing at an ERN that no longer exists, so stop it or move it with \"ets update-server\" first.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::RenameBucketRequest request;
        request.ern = vm["bucket"].as<std::string>();
        request.newName = vm["new-name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "rename-bucket", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: rename-bucket failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::enableEncryption(const std::vector<std::string> &args) const {
        po::options_description desc("enable encryption options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "ERN of the bucket to encrypt")
                ("key,k", po::value<std::string>(), "ID or ERN of the EKM key to encrypt under; a key is created if this is left out");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "enable-encryption", "--bucket <name|ern> [--key <id>]",
                                   "Encrypts the objects written to a bucket from now on. This applies to new uploads and puts only. "
                                   "Each one is encrypted with the bucket's "
                                   "EKM key before it reaches the disk and decrypted on the way back out, so uploads and downloads "
                                   "are unchanged - what changes is that the files under the storage directory are no longer the "
                                   "objects. Without --key a fresh AES-256 key is created for the bucket and belongs to EKM like any "
                                   "other: \"ekm list-keys\" shows it, and deleting it is what makes the bucket's objects "
                                   "unrecoverable. "
                                   "Objects the bucket already holds are left exactly as they are - they stay readable, but they stay "
                                   "as they were stored, and the answer says how many there are; re-upload or copy them through a new "
                                   "bucket if they have to be encrypted too. "
                                   "Enabling it again with another key rotates: new objects go under the new key, existing ones keep "
                                   "the key they name.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::EnableEncryptionRequest request;
        request.bucketErn = vm["bucket"].as<std::string>();
        if (vm.contains("key")) request.keyId = vm["key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "enable-encryption", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: enable-encryption failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::disableEncryption(const std::vector<std::string> &args) const {
        po::options_description desc("disable encryption options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "ERN of the bucket to stop encrypting");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "disable-encryption", "--bucket <name|ern>",
                                   "Stops encrypting the objects written to a bucket. This applies to new uploads and puts only: "
                                   "from here on they are stored in the clear, and nothing else changes. "
                                   "Objects already in the bucket are NOT decrypted and NOT rewritten - each one still names the key "
                                   "it was written under and is still decrypted on the way out, so downloads keep working exactly as "
                                   "before. The answer says how many objects are still stored encrypted. "
                                   "The bucket's EKM key is left untouched - not revoked, not scheduled for deletion - because those "
                                   "objects are under it, and deleting it with \"ekm delete-key\" is what would make them "
                                   "unrecoverable. Retire it only once nothing in the bucket is encrypted any more.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::DisableEncryptionRequest request;
        request.bucketErn = vm["bucket"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "disable-encryption", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: disable-encryption failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::renameObject(const std::vector<std::string> &args) const {
        po::options_description desc("rename object options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "ERN of the bucket the object is in")
                ("key,k", po::value<std::string>()->required(), "current key of the object")
                ("new-key,n", po::value<std::string>()->required(), "key it should have");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "rename-object", "--bucket <name|ern> --key <key> --new-key <key>",
                                   "Renames an object within its bucket. The same thing as \"move-object\" with one bucket, said the way it "
                                   "is usually meant: nothing is copied, and the object keeps its content type, checksum and attributes. "
                                   "Refuses rather than overwriting if something is already stored under the new key.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::RenameObjectRequest request;
        request.bucketErn = vm["bucket"].as<std::string>();
        request.key = vm["key"].as<std::string>();
        request.newKey = vm["new-key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "rename-object", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: rename-object failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::addObjectAttribute(const std::vector<std::string> &args) const {
        po::options_description desc("add object attribute options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "object ERN")
                ("name,n", po::value<std::string>()->required(), "attribute name")
                ("value,v", po::value<std::string>()->required(), "attribute value")
                ("type,t", po::value<std::string>()->default_value("string"), "attribute type");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "add-object-attribute", "--ern <ern> --name <name> --value <value> [--type <type>]",
                                   "Adds an attribute to an object. The object must not have an attribute of that name yet - use "
                                   "set-object-attribute to change one that exists. The type can be one of int, long, double, float, "
                                   "bool, string, binary, and defaults to string.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::ObjectAttributeRequest request;
        request.ern = vm["ern"].as<std::string>();
        request.name = vm["name"].as<std::string>();
        request.value = optionToVariant(vm["value"].as<std::string>(), vm["type"].as<std::string>());

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "add-object-attribute", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: add-object-attribute failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::setObjectAttribute(const std::vector<std::string> &args) const {
        po::options_description desc("set object attribute options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "object ERN")
                ("name,n", po::value<std::string>()->required(), "attribute name")
                ("value,v", po::value<std::string>()->required(), "attribute value")
                ("type,t", po::value<std::string>()->default_value("string"), "attribute type");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "set-object-attribute", "--ern <ern> --name <name> --value <value> [--type <type>]",
                                   "Sets the value of an existing object attribute. The attribute must exist already - use "
                                   "add-object-attribute to create one. The type can be one of int, long, double, float, bool, "
                                   "string, binary, and defaults to string.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::ObjectAttributeRequest request;
        request.ern = vm["ern"].as<std::string>();
        request.name = vm["name"].as<std::string>();
        request.value = optionToVariant(vm["value"].as<std::string>(), vm["type"].as<std::string>());

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "set-object-attribute", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: set-object-attribute failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::listObjectAttributes(const std::vector<std::string> &args) const {
        po::options_description desc("list object attributes options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "object ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "list-object-attributes", "--ern <ern>",
                                   "Lists every attribute of an object, with each value's type. An object with no attributes "
                                   "lists an empty set rather than failing.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::ListObjectAttributesRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "list-object-attributes", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-object-attributes failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::deleteObjectAttribute(const std::vector<std::string> &args) const {
        po::options_description desc("delete object attribute options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "object ERN")
                ("name,n", po::value<std::string>()->required(), "attribute name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "delete-object-attribute", "--ern <ern> --name <name>",
                                   "Deletes an attribute from an object. The attribute must exist; deleting one that was never "
                                   "stored is reported as an error rather than silently succeeding.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESM::DeleteObjectAttributeRequest request;
        request.ern = vm["ern"].as<std::string>();
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "delete-object-attribute", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-object-attribute failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::subscribe(const std::vector<std::string> &args) const {
        po::options_description desc("subscribe options");
        desc.add_options()
                ("source-ern,s", po::value<std::string>()->required(), "source bucket ERN")
                ("type,t", po::value<std::string>()->default_value("SQS"), "subscription type: SQS or SNS")
                ("target-ern,q", po::value<std::string>()->required(), "target ERN (an EQS queue ERN for SQS, an ENS topic ERN for SNS)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "subscribe", "--source-ern <bucketErn> --target-ern <queueErn|topicErn> [--type SQS|SNS]",
                                   "Subscribes a target resource to a bucket, so a notification is sent to the target "
                                   "every time an object is created in the bucket. Type SQS (the default) delivers "
                                   "straight to an EQS queue; type SNS delivers to an ENS topic, from where it fans "
                                   "out further to that topic's own SQS subscribers.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::SubscribeRequest request;
        request.sourceErn = vm["source-ern"].as<std::string>();
        request.type = vm["type"].as<std::string>();
        request.targetErn = vm["target-ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "subscribe", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: subscribe failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::unsubscribe(const std::vector<std::string> &args) const {
        po::options_description desc("unsubscribe options");
        desc.add_options()
                ("subscription,s", po::value<std::string>()->required(), "subscription ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "unsubscribe", "--subscription <ern>",
                                   "Deletes a subscription, identified by the ERN returned by euclid-cli-esm-subscribe(1). "
                                   "Deleting an ERN with no matching subscription is not an error.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::UnsubscribeRequest request;
        request.ern = vm["subscription"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "unsubscribe", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: unsubscribe failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EsmCli::listSubscriptions(const std::vector<std::string> &args) const {
        po::options_description desc("list subscriptions options");
        desc.add_options()
                ("bucket,b", po::value<std::string>()->required(), "bucket name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "list-subscriptions", "--bucket <name|ern>",
                                   "Lists the subscriptions of a bucket, identified by its Euclid resource name (ERN).",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::ESM::ListSubscriptionsRequest request;
        request.bucketErn = vm["bucket"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "list-subscriptions", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-subscriptions failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }
}