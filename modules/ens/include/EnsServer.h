#pragma once

#ifdef _WIN32
#undef SendMessageA
#endif

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
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/ens/Topic.h>
#include <euclid/database/entity/eqs/Queue.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ens/CreateTopicRequest.h>
#include <euclid/dto/ens/CreateTopicResponse.h>
#include <euclid/dto/ens/DeleteTopicRequest.h>
#include <euclid/dto/ens/EnsMapper.h>
#include <euclid/dto/ens/GetTopicErnRequest.h>
#include <euclid/dto/ens/GetTopicErnResponse.h>
#include <euclid/dto/ens/ListMessagesRequest.h>
#include <euclid/dto/ens/ListMessagesResponse.h>
#include <euclid/dto/ens/ListTopicsRequest.h>
#include <euclid/dto/ens/ListTopicsResponse.h>
#include <euclid/dto/ens/PublishMessageRequest.h>
#include <euclid/dto/ens/PublishMessageResponse.h>
#include <euclid/dto/ens/PurgeTopicRequest.h>

namespace Euclid::ENS {

    using namespace boost::beast::http;

    /**
     * @brief ENS service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EnsServer final : public Core::HttpActionServer {
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
        explicit EnsServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the background visibility timeout task.
         */
        ~EnsServer() override;

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
        std::string _resetMessagesTaskId;
    };

}// namespace Euclid::EQS