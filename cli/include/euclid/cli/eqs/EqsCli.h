#pragma once

// C++ includes
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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
#include <euclid/dto/eqs/CreateQueueResponse.h>
#include <euclid/dto/eqs/DeleteQueueRequest.h>
#include <euclid/dto/eqs/GetMessageAttributeRequest.h>
#include <euclid/dto/eqs/GetMessageCountRequest.h>
#include <euclid/dto/eqs/GetMessageMetadataRequest.h>
#include <euclid/dto/eqs/GetQueueMetadataRequest.h>
#include <euclid/dto/eqs/ListMessagesRequest.h>
#include <euclid/dto/eqs/ListMessagesResponse.h>
#include <euclid/dto/eqs/ListQueueRequest.h>
#include <euclid/dto/eqs/ListQueueResponse.h>
#include <euclid/dto/eqs/PurgeAllQueuesRequest.h>
#include <euclid/dto/eqs/PurgeQueueRequest.h>
#include <euclid/dto/eqs/ReceiveMessagesRequest.h>
#include <euclid/dto/eqs/SendMessageRequest.h>
#include <euclid/dto/eqs/SetMessageVisibilityRequest.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "access" module (e.g. "access login --user <u> --password <p>").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EqsCli {

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
        explicit EqsCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Create a new queue
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int createQueue(const std::vector<std::string> &args) const;

        /**
         * @brief Return the queue ERN
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getQueueErn(const std::vector<std::string> &args) const;

        /**
         * @brief List all available queues.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int listQueues(const std::vector<std::string> &args) const;

        /**
         * @brief Lists a queue's messages without receiving them, paginated.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int listMessages(const std::vector<std::string> &args) const;

        /**
         * @brief Purge a queue by deleting all messages.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int purgeQueue(const std::vector<std::string> &args) const;

        /**
         * @brief Purge all available queues.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int purgeAllQueues(const std::vector<std::string> &args) const;

        /**
         * @brief Returns the metadata of a queue
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int getQueueMetadata(const std::vector<std::string> &args) const;

        /**
         * @brief Delete a queue
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteQueue(const std::vector<std::string> &args) const;

        /**
         * @brief Send a message to the queue
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int sendMessage(const std::vector<std::string> &args) const;

        /**
         * @brief Receives message.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int receiveMessages(const std::vector<std::string> &args) const;

        /**
         * @brief Sets the visibility timeout for an individual message.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int setVisibility(const std::vector<std::string> &args) const;

        /**
         * @brief Returns the message count.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int getMessageCount(const std::vector<std::string> &args) const;

        /**
         * @brief Returns a single message attribute by name.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int getMessageAttribute(const std::vector<std::string> &args) const;

        /**
         * @brief Returns a message's metadata, i.e. queueErn, size, status, etc.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int getMessageMetadata(const std::vector<std::string> &args) const;

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