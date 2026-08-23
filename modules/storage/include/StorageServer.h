#pragma once

// C++ includes
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

// Boost includes
#include <boost/json.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/storage/Bucket.h>
#include <euclid/database/entity/storage/Object.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/storage/CompleteUploadRequest.h>
#include <euclid/dto/storage/CompleteUploadResponse.h>
#include <euclid/dto/storage/CreateBucketRequest.h>
#include <euclid/dto/storage/CreateBucketResponse.h>
#include <euclid/dto/storage/CreateUploadRequest.h>
#include <euclid/dto/storage/CreateUploadResponse.h>
#include <euclid/dto/storage/DeleteBucketRequest.h>
#include <euclid/dto/storage/GetBucketErnRequest.h>
#include <euclid/dto/storage/GetBucketErnResponse.h>
#include <euclid/dto/storage/ListBucketsRequest.h>
#include <euclid/dto/storage/ListBucketsResponse.h>
#include <euclid/dto/storage/StorageMapper.h>
#include <euclid/dto/storage/UploadPartResponse.h>

namespace Euclid::Storage {

    using namespace boost::beast::http;

    /**
     * @brief Storage service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class StorageServer final : public Core::HttpActionServer {
    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit StorageServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the background visibility timeout task.
         */
        ~StorageServer() override;

    protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;

    private:

        /**
         * @brief Id of the periodic task that resets expired-visibility messages, used to cancel
         * it on destruction.
         */
        std::string _lifecyleTaskId;
    };

}// namespace Euclid::Storage