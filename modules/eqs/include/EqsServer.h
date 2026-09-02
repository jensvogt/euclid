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
#include <euclid/core/LongPollSlots.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eqs/Queue.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eqs/AddQueueTagRequest.h>
#include <euclid/dto/eqs/CreateQueueRequest.h>
#include <euclid/dto/eqs/CreateQueueResponse.h>
#include <euclid/dto/eqs/DeleteMessageRequest.h>
#include <euclid/dto/eqs/DeleteQueueRequest.h>
#include <euclid/dto/eqs/DeleteQueueTagRequest.h>
#include <euclid/dto/eqs/mapper/EqsMapper.h>
#include <euclid/dto/eqs/GetMessageAttributeRequest.h>
#include <euclid/dto/eqs/GetMessageAttributeResponse.h>
#include <euclid/dto/eqs/GetMessageCountRequest.h>
#include <euclid/dto/eqs/GetMessageCountResponse.h>
#include <euclid/dto/eqs/GetMessageMetadataRequest.h>
#include <euclid/dto/eqs/GetMessageMetadataResponse.h>
#include <euclid/dto/eqs/GetQueueErnRequest.h>
#include <euclid/dto/eqs/GetQueueErnResponse.h>
#include <euclid/dto/eqs/GetQueueMetadataRequest.h>
#include <euclid/dto/eqs/GetQueueMetadataResponse.h>
#include <euclid/dto/eqs/ListMessagesRequest.h>
#include <euclid/dto/eqs/ListMessagesResponse.h>
#include <euclid/dto/eqs/ListQueueRequest.h>
#include <euclid/dto/eqs/ListQueueResponse.h>
#include <euclid/dto/eqs/PurgeAllQueuesRequest.h>
#include <euclid/dto/eqs/PurgeQueueRequest.h>
#include <euclid/dto/eqs/ReceiveMessagesRequest.h>
#include <euclid/dto/eqs/ReceiveMessagesResponse.h>
#include <euclid/dto/eqs/SendMessageRequest.h>
#include <euclid/dto/eqs/SendMessageResponse.h>
#include <euclid/dto/eqs/SetMessageAttributeRequest.h>
#include <euclid/dto/eqs/SetMessageVisibilityRequest.h>

namespace Euclid::EQS {

    using namespace boost::beast::http;

    /**
     * @brief EQS service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EqsServer final : public Core::HttpActionServer {
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
        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads number of io_context worker threads. Higher than the other modules
         * because this one's busiest action deliberately does nothing for up to twenty seconds
         * while it waits, holding a thread throughout: two threads means two waiting clients are
         * all it takes for everything else to queue. Core::LongPollSlots keeps one of these free
         * for requests that are not waiting, so this figure is how many may wait at once, plus
         * one.
         */
        explicit EqsServer(std::string socketPath, int threads = 8);

        /**
         * @brief Cancels the background visibility timeout task.
         */
        ~EqsServer() override;

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