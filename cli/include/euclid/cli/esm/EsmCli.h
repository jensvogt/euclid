#pragma once

// C++ includes
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Boost includes
#include <boost/json.hpp>
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/cli/BaseCli.h>
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/help/CliHelp.h>
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/core/ZipUtils.h>
#include <euclid/dto/eqs/CreateQueueRequest.h>
#include <euclid/dto/eqs/DeleteQueueTagRequest.h>
#include <euclid/dto/esm/AddBucketTagRequest.h>
#include <euclid/dto/esm/CompleteDownloadRequest.h>
#include <euclid/dto/esm/CompleteUploadRequest.h>
#include <euclid/dto/esm/CopyObjectRequest.h>
#include <euclid/dto/esm/CreateBucketRequest.h>
#include <euclid/dto/esm/CreateDownloadRequest.h>
#include <euclid/dto/esm/CreateDownloadResponse.h>
#include <euclid/dto/esm/CreateUploadRequest.h>
#include <euclid/dto/esm/CreateUploadResponse.h>
#include <euclid/dto/esm/DeleteBucketRequest.h>
#include <euclid/dto/esm/DeleteObjectAttributeRequest.h>
#include <euclid/dto/esm/DeleteObjectRequest.h>
#include <euclid/dto/esm/DisableEncryptionRequest.h>
#include <euclid/dto/esm/EnableEncryptionRequest.h>
#include <euclid/dto/esm/GetBucketErnRequest.h>
#include <euclid/dto/esm/GetBucketSizeRequest.h>
#include <euclid/dto/esm/GetObjectCountRequest.h>
#include <euclid/dto/esm/ListBucketsRequest.h>
#include <euclid/dto/esm/ListObjectAttributesRequest.h>
#include <euclid/dto/esm/ListObjectsRequest.h>
#include <euclid/dto/esm/ListObjectsResponse.h>
#include <euclid/dto/esm/ListSubscriptionsRequest.h>
#include <euclid/dto/esm/ObjectAttributeRequest.h>
#include <euclid/dto/esm/PurgeBucketRequest.h>
#include <euclid/dto/esm/RenameBucketRequest.h>
#include <euclid/dto/esm/RenameObjectRequest.h>
#include <euclid/dto/esm/SetBucketTagRequest.h>
#include <euclid/dto/esm/SubscribeRequest.h>
#include <euclid/dto/esm/UnsubscribeRequest.h>

// Fallbacks used when euclid.modules.esm.part-size/concurrency aren't set in the loaded
// configuration file (see main.cpp's --config) - so upload-file/download-file still have sane
// defaults when running against a bare install with no config, or none of these keys set.
// Every part but the last must be at least 5MB, which also keeps the default part count - and
// thus HTTP request overhead - reasonable for large files without ballooning per-part memory usage.
#define DEFAULT_PART_SIZE (5 * 1024 * 1024)
#define DEFAULT_CONCURRENCY 4

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "esm" module (e.g. "esm uploadFile --local ./file.txt --key file.txt").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EsmCli final : BaseCli {

    public:

        /**
         * @brief Constructs the handler.
         *
         * @param endpoint  Euclid server endpoint
         * @param authentication authentication including the bearer token used to authenticate requests, e.g. for register/list-users;
         * @param pretty pretty print output;
         * empty if the caller isn't logged in yet
         * @param caCertPath if non-empty, path to a PEM CA certificate trusted in addition to the
         * system trust store, e.g. for self-signed development certificates
         */
        explicit EsmCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

        /**
         * @brief Dispatches to the handler for the given action. Returns the process exit code.
         *
         * @param action action string
         * @param args list of action arguments
         */
        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

        /**
         * @brief Uploads one local file to one bucket/key - the per-file logic shared by
         * uploadFile() (its single --file) and uploadDirectory() (every file it finds), so both
         * pick single-request vs multipart the same way, based on the file's size relative to
         * partSize; only how the (bucketErn, key, filePath) triple is produced differs between
         * the two callers.
         *
         * Public because uploading a file is not only an "esm" action: "eap redeploy-application"
         * puts a new build where an application's artifact already lives, and that has to be the
         * same upload - multipart and all - rather than a second implementation of it.
         *
         * @param bucketErn ERN of the target bucket
         * @param key destination key (path) within the bucket
         * @param filePath local file to upload
         * @param partSize files at or above this size are split into parts and sent via
         * create-upload/upload-part/complete-upload; smaller files go through putObject() instead.
         * Zero or less selects the configured default
         * @param concurrency number of parts to upload in parallel, for files that go the
         * multipart route; zero or less selects the configured default
         * @param outResult set to the uploaded object's metadata (put-object's or
         * complete-upload's response) on success; untouched on failure
         * @return 0 on success, 1 on failure (error already printed to stderr)
         */
        [[nodiscard]]
        int uploadOneFile(const std::string &bucketErn, const std::string &key, const std::string &filePath, long partSize, int concurrency, boost::json::value &outResult) const;

    private:

        /**
         * @brief Create a new bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int createBucket(const std::vector<std::string> &args) const;

        /**
         * @brief Delete a bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteBucket(const std::vector<std::string> &args) const;

        /**
         * @brief Gives a bucket another name, rewriting everything that names it.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int renameBucket(const std::vector<std::string> &args) const;

        /**
         * @brief Turns on encryption at rest for a bucket, under a named or newly created EKM key.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int enableEncryption(const std::vector<std::string> &args) const;

        /**
         * @brief Stops encrypting the objects written to a bucket. Applies to new uploads and puts
         * only - what is already stored stays encrypted, and its EKM key is left untouched.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int disableEncryption(const std::vector<std::string> &args) const;

        /**
         * @brief List buckets
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listBuckets(const std::vector<std::string> &args) const;

        /**
         * @brief Return the bucket ERN
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getBucketErn(const std::vector<std::string> &args) const;

        /**
         * @brief Return the bucket size
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getBucketSize(const std::vector<std::string> &args) const;

        /**
         * @brief Uploads a local file to a bucket. The only upload action exposed to the user;
         * internally splits the file into parts and drives create-upload/upload-part/complete-upload
         * so multipart upload is invisible to the caller.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int uploadFile(const std::vector<std::string> &args) const;

        /**
         * @brief Uploads every file in a local directory to a bucket, one object per file. Each
         * file's object key is the given prefix followed by its path relative to the directory
         * (with forward slashes regardless of platform). Without --recursive, only files directly
         * inside the directory are uploaded; with it, subdirectories are walked too. Each file is
         * uploaded via uploadOneFile(), the same put-object/multipart logic uploadFile() uses for
         * its single --file.
         *
         * @param args command line arguments
         * @return ok (0 only if every file uploaded successfully)
         */
        [[nodiscard]]
        int uploadDirectory(const std::vector<std::string> &args) const;

        /**
         * @brief Downloads an object from a bucket to a local file. The mirror image of
         * uploadFile(); internally drives create-download/download-part/complete-download so
         * multipart download is invisible to the caller.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int downloadFile(const std::vector<std::string> &args) const;

        /**
         * @brief Downloads objects from a bucket to a local directory, one file per object. Each
         * local file's path is the given directory followed by the object's key with the given
         * prefix stripped off the front. Without --recursive, only an object's direct
         * descendants are downloaded (keys with no further '/' after the prefix); with it, every
         * key matching the prefix is downloaded, however deeply nested. Each object is downloaded
         * via downloadOneFile(), the same get-object/multipart logic downloadFile() uses for its
         * single --key. If --zip is given, --dir's contents are additionally packed into a single
         * ZIP archive at that path via Core::ZipUtils::Zip(), once every object has been
         * downloaded; the downloaded files themselves are left in place under --dir.
         *
         * @param args command line arguments
         * @return ok (0 only if every matching object downloaded successfully and, if requested, the zip was written)
         */
        [[nodiscard]]
        int downloadBucket(const std::vector<std::string> &args) const;

        /**
         * @brief List all objects of a bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listObjects(const std::vector<std::string> &args) const;

        /**
         * @brief Returns the number of objects in a bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getObjectCount(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes an object
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteObject(const std::vector<std::string> &args) const;

        /**
         * @brief Removes all objects from a bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int purgeBucket(const std::vector<std::string> &args) const;

        /**
         * @brief Adds a bucket tag
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int addBucketTag(const std::vector<std::string> &args) const;

        /**
         * @brief Sets the value of an existing bucket tag
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int setBucketTag(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes a bucket tag
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteBucketTag(const std::vector<std::string> &args) const;

        /**
         * @brief Copies or moves an object; the two differ only in whether the source survives.
         *
         * @param args command line arguments
         * @param keepSource true to copy, false to move
         * @return ok
         */
        [[nodiscard]]
        int transferObject(const std::vector<std::string> &args, bool keepSource) const;

        /**
         * @brief Copies an object to another key or bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int copyObject(const std::vector<std::string> &args) const;

        /**
         * @brief Moves an object to another key or bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int moveObject(const std::vector<std::string> &args) const;

        /**
         * @brief Renames an object within its bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int renameObject(const std::vector<std::string> &args) const;

        /**
         * @brief Adds an attribute to an object, which must not have one of that name yet
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int addObjectAttribute(const std::vector<std::string> &args) const;

        /**
         * @brief Sets the value of an existing object attribute
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int setObjectAttribute(const std::vector<std::string> &args) const;

        /**
         * @brief Lists the attributes of an object
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listObjectAttributes(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes an attribute from an object
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteObjectAttribute(const std::vector<std::string> &args) const;

        /**
         * @brief Subscribes a target resource (an EQS queue) to a bucket's object-created events.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int subscribe(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes a subscription.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int unsubscribe(const std::vector<std::string> &args) const;

        /**
         * @brief Lists the subscriptions of a bucket.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listSubscriptions(const std::vector<std::string> &args) const;

        /**
         * @brief Stores a small object in a single request, skipping the
         * create-upload/upload-part/complete-upload sequence entirely (internal helper used by
         * uploadFile() for files under its part size; not a standalone CLI action).
         *
         * Like uploadPart(), this does NOT send a JSON body - bucketErn/key travel as headers and
         * the file's bytes go straight over the wire as raw data (see HttpClient::PostBinary()).
         * Unlike uploadPart(), the response IS JSON: the completed object's metadata, the same
         * shape complete-upload returns.
         *
         * @param bucketErn ERN of the target bucket
         * @param key destination key (path) within the bucket
         * @param data the whole file's bytes
         * @return the parsed JSON response, or empty if the request failed (error already printed to stderr)
         */
        [[nodiscard]]
        std::optional<boost::json::value> putObject(const std::string &bucketErn, const std::string &key, const std::string &data) const;

        /**
         * @brief Starts a multipart upload (internal helper used by uploadFile; not a standalone
         * CLI action).
         *
         * @param bucketErn ERN of the target bucket
         * @param key destination key (path) within the bucket
         * @param concurrency number of parts the caller intends to upload in parallel; sent as an
         * "x-euclid-expected-concurrency" header so the autoscaler can proactively scale toward it
         * @return upload ID, or empty if the request failed (error already printed to stderr)
         */
        [[nodiscard]]
        std::optional<std::string> createUpload(const std::string &bucketErn, const std::string &key, int concurrency) const;

        /**
         * @brief Uploads one part of a multipart upload (internal helper used by uploadFile; not a
         * standalone CLI action).
         *
         * Unlike every other action here, this does NOT send a JSON body - uploadId/partNumber
         * travel as headers and data goes straight over the wire as raw bytes (see
         * HttpClient::PostBinary()). Parts are high-volume and internal-only, so the base64-in-JSON
         * overhead every other action accepts for readability isn't worth paying here.
         *
         * Retries a few times with backoff on network failures or a 5xx response before giving up,
         * since parts are the hot path of an upload (thousands of calls for a large file) and thus
         * the most likely to hit a transient failure - e.g. a request landing on a storage instance
         * the gateway's autoscaler is mid-way through killing.
         *
         * @param uploadId upload ID returned by createUpload()
         * @param partNumber 1-based part number
         * @param data raw part bytes
         * @return true on success (error already printed to stderr on failure)
         */
        [[nodiscard]]
        bool uploadPart(const std::string &uploadId, long partNumber, const std::string &data) const;

        /**
         * @brief Completes a multipart upload, assembling its parts into the final object
         * (internal helper used by uploadFile; not a standalone CLI action).
         *
         * @param uploadId upload ID returned by createUpload()
         * @return the parsed JSON response, or empty if the request failed (error already printed to stderr)
         */
        [[nodiscard]]
        std::optional<boost::json::value> completeUpload(const std::string &uploadId) const;

        /**
         * @brief Attempts to fetch a small object's full bytes in a single request (internal
         * helper used by downloadFile(); not a standalone CLI action). Mirrors putObject(): no
         * download session is created, this looks the object up by bucket/key directly, the same
         * way put-object does for the write side.
         *
         * Unlike uploadFile() (which stats the local file upfront and so knows before making any
         * request whether it's small), a download's size isn't known until asked - so
         * downloadFile() always tries this first, and the server enforces maxInlineSize itself,
         * responding with HTTP 413 if the object turns out to be too large. The caller checks the
         * returned status code for that to fall back to the create-download/download-part/
         * complete-download sequence instead; any other non-success status is left for the caller
         * to report, since only it knows whether that's worth printing as an error.
         *
         * A network/transport failure is reported here (like every other helper here does) rather
         * than left to the caller, since there's no HTTP status to hand back for the caller to
         * interpret in that case - signaled by a statusCode of 0.
         *
         * @param bucketErn ERN of the source bucket
         * @param key key (path) of the object within the bucket
         * @param maxInlineSize objects at or above this size are rejected (HTTP 413) rather than
         * fetched, so the caller can fall back to a multipart download instead of this ever
         * returning an unbounded response body
         * @return the raw response; statusCode is 0 on a network/transport failure (already
         * printed to stderr), 413 if the object was too large, or the backend's actual HTTP status
         * otherwise
         */
        [[nodiscard]]
        BinaryHttpResponse getObject(const std::string &bucketErn, const std::string &key, long maxInlineSize) const;

        /**
         * @brief Starts a multipart download (internal helper used by downloadFile; not a
         * standalone CLI action). The mirror image of createUpload(): rather than staging an
         * empty upload, it looks up the object being downloaded and returns its size/content
         * type alongside the download ID, so the caller knows how many parts to request.
         *
         * @param bucketErn ERN of the source bucket
         * @param key key (path) of the object within the bucket
         * @param concurrency number of parts the caller intends to download in parallel; sent as
         * an "x-euclid-expected-concurrency" header so the autoscaler can proactively scale toward
         * it, same as createUpload()
         * @return the parsed create-download response, or empty if the request failed (error
         * already printed to stderr)
         */
        [[nodiscard]]
        std::optional<Dto::ESM::CreateDownloadResponse> createDownload(const std::string &bucketErn, const std::string &key, int concurrency) const;

        /**
         * @brief Downloads one part of a multipart download and writes it at the right offset in
         * the destination file (internal helper used by downloadFile; not a standalone CLI
         * action). The mirror image of uploadPart(): download ID/part number/part size travel as
         * headers (see HttpClient::PostForBinary()), and the raw bytes come back in the response
         * body instead of JSON.
         *
         * Retries a few times with backoff on network failures or a 5xx response before giving
         * up, same as uploadPart() and for the same reason.
         *
         * @param downloadId download ID returned by createDownload()
         * @param partNumber 1-based part number
         * @param partSize size in bytes of every part but possibly the last (used to compute this
         * part's byte offset into the object)
         * @param filePath path of the local destination file, pre-sized to the object's full
         * length before any part is requested
         * @return true on success (error already printed to stderr on failure)
         */
        [[nodiscard]]
        bool downloadPart(const std::string &downloadId, long partNumber, long partSize, const std::string &filePath) const;

        /**
         * @brief Completes a multipart download, discarding its server-side scratch state
         * (internal helper used by downloadFile; not a standalone CLI action).
         *
         * @param downloadId download ID returned by createDownload()
         * @return true on success (error already printed to stderr on failure)
         */
        [[nodiscard]]
        bool completeDownload(const std::string &downloadId) const;

        /**
         * @brief Downloads one bucket/key to one local file - the per-file logic shared by
         * downloadFile() (its single --key) and downloadBucket() (every matching key it finds),
         * so both pick single-request vs multipart the same way. Creates filePath's parent
         * directories first, since downloadBucket()'s keys can be nested arbitrarily deep under
         * directories that don't exist locally yet.
         *
         * @param bucketErn ERN of the source bucket
         * @param key key (path) of the object within the bucket
         * @param filePath local destination file; overwritten if it already exists
         * @param partSize objects at or above this size are fetched via
         * create-download/download-part/complete-download; smaller ones via getObject() instead
         * @param concurrency number of parts to download in parallel, for objects that go the
         * multipart route
         * @param outResult set to the downloaded file's summary metadata on success; untouched
         * on failure
         * @return 0 on success, 1 on failure (error already printed to stderr)
         */
        [[nodiscard]]
        int downloadOneFile(const std::string &bucketErn, const std::string &key, const std::string &filePath, long partSize, int concurrency, boost::json::value &outResult) const;

        /**
         * @brief Fetches every object matching bucketErn/prefix, paging through list-objects
         * until exhausted (internal helper used by downloadBucket(); not a standalone CLI
         * action).
         *
         * @param bucketErn ERN of the bucket to list
         * @param prefix only objects whose key starts with this are returned; empty matches all
         * @return every matching object, or empty if the request failed (error already printed to stderr)
         */
        [[nodiscard]]
        std::optional<std::vector<Dto::ESM::Object> > listAllObjects(const std::string &bucketErn, const std::string &prefix) const;

        /**
         * @brief Euclid endpoint
         */
        std::string _endpoint;

        /**
         * @brief Bearer token used to authenticate requests, or empty if not logged in.
         */
        Credentials::Entry _authentication;

        /**
         * @brief Pretty print flag
         */
        bool _pretty;

        /**
         * @brief Path to an additional PEM CA certificate to trust, or empty to use only the
         * system trust store.
         */
        std::string _caCertPath;
    };

}