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

        // uploadPart() retry tuning: parts are the hot path of an upload (thousands of calls for a
        // large file), so they're the ones most likely to hit a transient failure - e.g. a request
        // landing on a storage instance the gateway's autoscaler is mid-way through killing. A
        // handful of quick retries turns that into a brief stall instead of aborting the whole
        // upload.
        constexpr int kMaxPartAttempts = 4;
        constexpr std::chrono::milliseconds kPartRetryBaseDelay{500};

        // upload-file/download-file's --part-size/--concurrency defaults, overridable per
        // deployment via euclid.modules.storage.part-size/concurrency in the loaded configuration
        // file (see main.cpp's --config) instead of only ever falling back to the DEFAULT_PART_SIZE/
        // DEFAULT_CONCURRENCY compiled-in constants.
        long DefaultPartSize() {
            return Core::Configuration::instance().getOr<long>("euclid.modules.storage.part-size", static_cast<long>(DEFAULT_PART_SIZE));
        }

        int DefaultConcurrency() {
            return Core::Configuration::instance().getOr<int>("euclid.modules.storage.concurrency", DEFAULT_CONCURRENCY);
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
                                           {"create-bucket", "Create a new bucket"},
                                           {"list-buckets", "List buckets"},
                                           {"get-bucket-ern", "Resolve a bucket's ERN by name"},
                                           {"get-bucket-size", "Returns the bucket size in bytes"},
                                           {"purge-bucket", "Removes all objects from a bucket"},
                                           {"delete-bucket", "Delete a bucket"},
                                           {"upload-file", "Upload a local file to a bucket"},
                                           {"upload-directory", "Upload every file in a local directory to a bucket"},
                                           {"download-file", "Download an object from a bucket to a local file"},
                                           {"download-bucket", "Download a bucket's objects to a local directory"},
                                           {"list-objects", "List objects"},
                                           {"get-object-count", "Return the number of objects in a bucket"},
                                           {"add-bucket-tag", "Adds a tag to a bucket"},
                                           {"set-bucket-tag", "Sets the value of an existing bucket tag"},
                                           {"delete-bucket-tag", "Deletes a tag from a bucket"},
                                           {"delete-object", "Deletes an object by ERN"},
                                           {"subscribe", "Subscribes a target resource (an EQS queue) to a bucket's object-created events"},
                                           {"unsubscribe", "Deletes a subscription"},
                                           {"list-subscriptions", "Lists the subscriptions of a bucket"},
                                   });
        }
        if (action == "create-bucket") {
            return createBucket(args);
        }
        if (action == "delete-bucket") {
            return deleteBucket(args);
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
                ("name,n", po::value<std::string>()->required(), "name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "create-bucket", "--name <name>",
                                   "Creates a new storage bucket with the given name.",
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
                ("bucket,b", po::value<std::string>()->required(), "bucketERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "delete-bucket", "--bucket <ern>",
                                   "Deletes a storage bucket identified by its Euclid resource name (ERN).",
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "delete-bucket", boost::json::value_from(request)); !response.IsSuccess()) {
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
                ("pageSize,s", po::value<long>()->default_value(-1), "page size")
                ("pageIndex,i", po::value<long>()->default_value(-1), "page index")
                ("sortColumn,c", po::value<std::string>()->default_value("name"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "list-buckets", "[--prefix <prefix>] [--pageSize <n>] [--pageIndex <n>] [--sortColumn <column>]",
                                   "Lists storage buckets, optionally filtered by name prefix and paginated.",
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
        request.pageSize = vm["pageSize"].as<long>();
        request.pageIndex = vm["pageIndex"].as<long>();
        request.sortColumn = vm["sortColumn"].as<std::string>();
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "get-bucket-size", "--bucket <ern>",
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "create-upload", boost::json::value_from(request), headers);
            if (!response.IsSuccess()) {
                std::cerr << "error: create-upload failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return std::nullopt;
            }
            return boost::json::value_to<Dto::ESM::CreateUploadResponse>(response.body).uploadId;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return std::nullopt;
        }
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "complete-upload", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: complete-upload failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return std::nullopt;
            }
            return response.body;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return std::nullopt;
        }
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "create-download", boost::json::value_from(request), headers);
            if (!response.IsSuccess()) {
                std::cerr << "error: create-download failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return std::nullopt;
            }
            return boost::json::value_to<Dto::ESM::CreateDownloadResponse>(response.body);
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return std::nullopt;
        }
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("esm", "complete-download", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: complete-download failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return false;
            }
            return true;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return false;
        }
    }

    int EsmCli::uploadOneFile(const std::string &bucketErn, const std::string &key, const std::string &filePath, const long partSize, const int concurrency, boost::json::value &outResult) const {
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
            return PrintActionHelp("esm", "upload-file", "--bucket <ern> --key <key> --file <path> [--part-size <bytes>] [--concurrency <n>]",
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
            return PrintActionHelp("esm", "upload-directory", "--bucket <ern> --dir <path> [--prefix <prefix>] [--recursive] [--part-size <bytes>] [--concurrency <n>]",
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
            return PrintActionHelp("esm", "download-file", "--bucket <ern> --key <key> --file <path> [--part-size <bytes>] [--concurrency <n>]",
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
            return PrintActionHelp("esm", "download-bucket", "--bucket <ern> --dir <path> [--prefix <prefix>] [--recursive] [--part-size <bytes>] [--concurrency <n>] [--zip <path>]",
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN")
                ("prefix,p", po::value<std::string>(), "bucket name prefix")
                ("pageSize,s", po::value<long>()->default_value(-1), "page size")
                ("pageIndex,i", po::value<long>()->default_value(-1), "page index")
                ("sortColumn,c", po::value<std::string>()->default_value("name"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "list-objects", "--bucket <ern> [--prefix <prefix>] [--pageSize <n>] [--pageIndex <n>] [--sortColumn <column>]",
                                   "Lists storage objects by bucket, optionally filtered by name prefix and paginated.",
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
        request.pageSize = vm["pageSize"].as<long>();
        request.pageIndex = vm["pageIndex"].as<long>();
        request.sortColumn = vm["sortColumn"].as<std::string>();
        if (vm.contains("bucket")) {
            request.bucketErn = vm["bucket"].as<std::string>();
        }
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN")
                ("prefix,p", po::value<std::string>(), "object key prefix");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "get-object-count", "--bucket <ern>",
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
        request.bucketErn = vm["bucket"].as<std::string>();
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN")
                ("prefix,p", po::value<std::string>(), "object key prefix");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "purge-bucket", "--bucket <ern> [--prefix <value>]",
                                   "Removes all objects from a bucket identified by its Euclid resource name (ERN), leaving the (empty) "
                                   "bucket itself in place, optionally filtered by object key prefix. It returns the ERN and the number of remaining objects.",
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
        request.bucketErn = vm["bucket"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("esm", "purge-bucket", boost::json::value_from(request));
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN")
                ("key,k", po::value<std::string>()->required(), "tag key")
                ("value,v", po::value<std::string>()->required(), "tag value");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "add-bucket-tag", "--bucket <ern> --key <value> --value <value>",
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
        request.bucketErn = vm["bucket"].as<std::string>();
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN")
                ("key,k", po::value<std::string>()->required(), "tag key")
                ("value,v", po::value<std::string>()->required(), "tag value");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "set-bucket-tag", "--bucket <ern> --key <value> --value <value>",
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
        request.bucketErn = vm["bucket"].as<std::string>();
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN")
                ("key,k", po::value<std::string>()->required(), "tag key");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "delete-bucket-tag", "--bucket <ern> --key <value>",
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
        request.queueErn = vm["bucket"].as<std::string>();
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

    int EsmCli::subscribe(const std::vector<std::string> &args) const {
        po::options_description desc("subscribe options");
        desc.add_options()
                ("source-ern,s", po::value<std::string>()->required(), "source bucket ERN")
                ("type,t", po::value<std::string>()->default_value("SQS"), "subscription type (only SQS is supported for now)")
                ("target-ern,q", po::value<std::string>()->required(), "target ERN (an EQS queue ERN)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "subscribe", "--source-ern <bucketErn> --target-ern <queueErn> [--type SQS]",
                                   "Subscribes a target resource to a bucket, so a notification is sent to the target "
                                   "every time an object is created in the bucket. Only type SQS is supported for now, "
                                   "so --target-ern must be the ERN of an EQS queue; --type defaults to SQS.",
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
                ("bucket,b", po::value<std::string>()->required(), "bucket ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "list-subscriptions", "--bucket <ern>",
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