#pragma once

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
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/access/User.h>
#include <euclid/database/entity/sqs/Queue.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/sqs/CreateQueueRequest.h>
#include <euclid/dto/sqs/CreateQueueResponse.h>
#include <euclid/dto/sqs/DeleteMessageRequest.h>
#include <euclid/dto/sqs/DeleteQueueRequest.h>
#include <euclid/dto/sqs/GetMessageAttributeRequest.h>
#include <euclid/dto/sqs/GetMessageAttributeResponse.h>
#include <euclid/dto/sqs/GetMessageCountRequest.h>
#include <euclid/dto/sqs/GetMessageCountResponse.h>
#include <euclid/dto/sqs/GetMessageMetadataRequest.h>
#include <euclid/dto/sqs/GetMessageMetadataResponse.h>
#include <euclid/dto/sqs/GetQueueErnRequest.h>
#include <euclid/dto/sqs/GetQueueErnResponse.h>
#include <euclid/dto/sqs/GetQueueMetadataRequest.h>
#include <euclid/dto/sqs/GetQueueMetadataResponse.h>
#include <euclid/dto/sqs/ListMessagesRequest.h>
#include <euclid/dto/sqs/ListMessagesResponse.h>
#include <euclid/dto/sqs/ListQueueRequest.h>
#include <euclid/dto/sqs/ListQueueResponse.h>
#include <euclid/dto/sqs/PurgeAllQueuesRequest.h>
#include <euclid/dto/sqs/PurgeQueueRequest.h>
#include <euclid/dto/sqs/ReceiveMessagesRequest.h>
#include <euclid/dto/sqs/ReceiveMessagesResponse.h>
#include <euclid/dto/sqs/SendMessageRequest.h>
#include <euclid/dto/sqs/SendMessageResponse.h>
#include <euclid/dto/sqs/SqsMapper.h>

namespace Euclid::SQS {

    using namespace boost::beast::http;

    /**
     * @brief SQS service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class SqsServer final : public Core::HttpActionServer {
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
        explicit SqsServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the background visibility timeout task.
         */
        ~SqsServer() override;

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

}// namespace Euclid::SQS