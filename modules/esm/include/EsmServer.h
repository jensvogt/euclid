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
#include <euclid/core/ContentTypeUtils.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/esm/Bucket.h>
#include <euclid/database/entity/esm/Object.h>
#include <euclid/database/entity/esm/ObjectStatus.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/esm/CompleteDownloadRequest.h>
#include <euclid/dto/esm/CompleteUploadRequest.h>
#include <euclid/dto/esm/CompleteUploadResponse.h>
#include <euclid/dto/esm/CreateBucketRequest.h>
#include <euclid/dto/esm/CreateBucketResponse.h>
#include <euclid/dto/esm/CreateDownloadRequest.h>
#include <euclid/dto/esm/CreateDownloadResponse.h>
#include <euclid/dto/esm/CreateUploadRequest.h>
#include <euclid/dto/esm/CreateUploadResponse.h>
#include <euclid/dto/esm/DeleteBucketRequest.h>
#include <euclid/dto/esm/DeleteObjectRequest.h>
#include <euclid/dto/esm/GetBucketErnRequest.h>
#include <euclid/dto/esm/GetBucketErnResponse.h>
#include <euclid/dto/esm/GetBucketSizeRequest.h>
#include <euclid/dto/esm/GetBucketSizeResponse.h>
#include <euclid/dto/esm/ListBucketsRequest.h>
#include <euclid/dto/esm/ListBucketsResponse.h>
#include <euclid/dto/esm/ListObjectsRequest.h>
#include <euclid/dto/esm/ListObjectsResponse.h>
#include <euclid/dto/esm/EsmMapper.h>
#include <euclid/dto/esm/PurgeBucketRequest.h>
#include <euclid/dto/esm/PurgeBucketResponse.h>
#include <euclid/dto/esm/UploadPartResponse.h>

namespace Euclid::ESM {

    using namespace boost::beast::http;

    /**
     * @brief Storage service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EsmServer final : public Core::HttpActionServer {
    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EsmServer(std::string socketPath, int threads = 2);

        ~EsmServer() override;

    protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;
    };

}// namespace Euclid::ESM