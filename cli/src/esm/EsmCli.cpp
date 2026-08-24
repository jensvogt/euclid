// Euclid includes
#include <euclid/cli/esm/EsmCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        /**
         * @brief Resolves a value that may be given literally or as "file://path", in which case it is read from the referenced file instead.
         */
        std::string ResolveFileOrLiteral(const std::string &value) {
            constexpr std::string_view prefix = "file://";
            if (!value.starts_with(prefix)) {
                return value;
            }
            const std::string path = value.substr(prefix.size());
            std::ifstream in(path);
            if (!in.is_open()) {
                throw std::runtime_error("could not open file '" + path + "'");
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

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

    }

    EsmCli::EsmCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EsmCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("esm", {
                                           {"create-bucket", "Create a new bucket"},
                                           {"delete-bucket", "Delete a bucket"},
                                           {"list-buckets", "List buckets"},
                                           {"get-bucket-ern", "Resolve a bucket's ERN by name"},
                                           {"get-bucket-size", "Returns the bucket size in bytes"},
                                           {"upload-file", "Upload a local file to a bucket"},
                                           {"list-objects", "List objects"},
                                           {"delete-object", "Deletes an object by ERN"},
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
        if (action == "list-objects") {
            return listObjects(args);
        }
        if (action == "delete-object") {
            return deleteObject(args);
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
                ("ern,e", po::value<std::string>()->required(), "euclid resource name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "delete-bucket", "--ern <ern>",
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
        request.ern = vm["ern"].as<std::string>();

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
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }
        request.pageSize = vm["pageSize"].as<long>();
        request.pageIndex = vm["pageIndex"].as<long>();
        request.sortColumn = vm["sortColumn"].as<std::string>();

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
                ("ern,e", po::value<std::string>()->required(), "euclid resource name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "get-bucket-size", "--ern <ern>",
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
        request.ern = vm["ern"].as<std::string>();

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

    int EsmCli::uploadFile(const std::vector<std::string> &args) const {
        po::options_description desc("upload file options");
        desc.add_options()
                ("bucket-ern,b", po::value<std::string>()->required(), "ERN of the target bucket")
                ("key,k", po::value<std::string>()->required(), "destination key (path) within the bucket")
                ("file,f", po::value<std::string>()->required(), "local file to upload")
                ("part-size,s", po::value<long>()->default_value(DEFAULT_UPLOAD_PART_SIZE), "part size in bytes")
                ("concurrency,j", po::value<int>()->default_value(DEFAULT_CONCURRENCY), "number of parts to upload in parallel");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("esm", "upload-file", "--bucket-ern <ern> --key <key> --file <path> [--part-size <bytes>] [--concurrency <n>]",
                                   "Uploads a local file to a bucket, transparently splitting it into parts for a multipart upload "
                                   "and uploading up to <concurrency> parts at a time in background threads.",
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

        const auto bucketErn = vm["bucket-ern"].as<std::string>();
        const auto key = vm["key"].as<std::string>();
        const auto filePath = vm["file"].as<std::string>();
        const auto partSize = vm["part-size"].as<long>();
        const auto concurrency = std::max(1, vm["concurrency"].as<int>());

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

        Core::WriteJson(std::cout, *result, _pretty);
        return 0;
    }

    int EsmCli::listObjects(const std::vector<std::string> &args) const {
        po::options_description desc("list objects options");
        desc.add_options()
                ("bucket,b", po::value<std::string>(), "bucket ERN")
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
        if (vm.contains("bucket")) {
            request.bucketErn = vm["bucket"].as<std::string>();
        }
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }
        request.pageSize = vm["pageSize"].as<long>();
        request.pageIndex = vm["pageIndex"].as<long>();
        request.sortColumn = vm["sortColumn"].as<std::string>();

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

}