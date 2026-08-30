#pragma once

// Boost includes
#include <boost/json.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/ekm/Key.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ekm/CreateKeyRequest.h>
#include <euclid/dto/ekm/CreateKeyResponse.h>
#include <euclid/dto/ekm/ListKeysRequest.h>
#include <euclid/dto/ekm/ListKeysResponse.h>

namespace Euclid::EKM {

    using namespace boost::beast::http;

    /**
     * @brief EKM service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EkmServer final : public Core::HttpActionServer {
    public:

        /**
         * @brief Constructs the server.
         *
         * Also starts the background task that periodically resets messages whose visibility
         * timeout has expired back to status "INITIAL" so they become receivable again.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EkmServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the background visibility timeout task.
         */
        ~EkmServer() override;

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

}// namespace Euclid::EKM