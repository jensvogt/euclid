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
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/help/CliHelp.h>
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/eqs/CreateQueueRequest.h>
#include <euclid/dto/esm/CompleteUploadRequest.h>
#include <euclid/dto/esm/CreateBucketRequest.h>
#include <euclid/dto/esm/CreateUploadRequest.h>
#include <euclid/dto/esm/CreateUploadResponse.h>
#include <euclid/dto/esm/DeleteBucketRequest.h>
#include <euclid/dto/esm/DeleteObjectRequest.h>
#include <euclid/dto/esm/GetBucketErnRequest.h>
#include <euclid/dto/esm/GetBucketSizeRequest.h>
#include <euclid/dto/esm/ListBucketsRequest.h>
#include <euclid/dto/esm/ListObjectsRequest.h>

#define DEFAULT_UPLOAD_PART_SIZE (5 * 1024 * 1024)
#define DEFAULT_CONCURRENCY 4

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "storage" module (e.g. "storage uploadFile --local ./file.txt --key file.txt").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class StorageCli {

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
        explicit StorageCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

        /**
         * @brief Dispatches to the handler for the given action. Returns the process exit code.
         *
         * @param action action string
         * @param args list of action arguments
         */
        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

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
         * @brief List all objects of a bucket
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listObjects(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes an object
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteObject(const std::vector<std::string> &args) const;

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