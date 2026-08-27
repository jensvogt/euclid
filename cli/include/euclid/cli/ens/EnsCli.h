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
#include <euclid/cli/BaseCli.h>
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/help/CliHelp.h>
#include <euclid/cli/help/CliHelp.h>
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>
#include <euclid/dto/ens/AddTopicTagRequest.h>
#include <euclid/dto/ens/CreateTopicRequest.h>
#include <euclid/dto/ens/CreateTopicResponse.h>
#include <euclid/dto/ens/DeleteTopicRequest.h>
#include <euclid/dto/ens/DeleteTopicTagRequest.h>
#include <euclid/dto/ens/GetMessageAttributeRequest.h>
#include <euclid/dto/ens/GetMessageCountRequest.h>
#include <euclid/dto/ens/GetTopicErnRequest.h>
#include <euclid/dto/ens/GetTopicMetadataRequest.h>
#include <euclid/dto/ens/ListMessagesRequest.h>
#include <euclid/dto/ens/ListTopicsRequest.h>
#include <euclid/dto/ens/PublishMessageRequest.h>
#include <euclid/dto/ens/PurgeAllTopicsRequest.h>
#include <euclid/dto/ens/PurgeTopicRequest.h>
#include <euclid/dto/ens/SetMessageAttributeRequest.h>
#include <euclid/dto/ens/SetQueueTagRequest.h>
#include <euclid/dto/ens/ListSubscriptionsRequest.h>
#include <euclid/dto/ens/SubscribeRequest.h>
#include <euclid/dto/ens/UnsubscribeRequest.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "ENS" module (e.g. "ens list-topics<p>").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EnsCli final : BaseCli {

    public:

        /**
         * @brief Constructs the handler.
         *
         * @param endpoint  Euclid server endpoint
         * @param authentication authentication including the bearer token used to authenticate requests, e.g. for register/list-users;
         * empty if the caller isn't logged in yet
         * @param pretty pretty print output;
         * @param caCertPath if non-empty, path to a PEM CA certificate trusted in addition to the
         * system trust store, e.g. for self-signed development certificates
         */
        explicit EnsCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Create a new topic
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int createTopic(const std::vector<std::string> &args) const;

        /**
         * @brief List all available topics.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int listTopics(const std::vector<std::string> &args) const;

        /**
         * @brief Return the topic ERN
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int getTopicErn(const std::vector<std::string> &args) const;

        /**
         * @brief Delete a topic
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteTopic(const std::vector<std::string> &args) const;

        /**
         * @brief Lists a topic's messages without receiving them, paginated.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int listMessages(const std::vector<std::string> &args) const;

        /**
         * @brief Purge a topic by deleting all messages.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int purgeTopic(const std::vector<std::string> &args) const;

        /**
         * @brief Purge all available topics.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int purgeAllTopic(const std::vector<std::string> &args) const;

        /**
         * @brief Returns the metadata of a topic
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int getTopicMetadata(const std::vector<std::string> &args) const;

        /**
         * @brief Send a message to the topic
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int publishMessage(const std::vector<std::string> &args) const;

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
         * @brief Sets a ENS message attribute
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int setMessageAttribute(const std::vector<std::string> &args) const;

        /**
         * @brief Returns a message's metadata, i.e. topicErn, size, status, etc.
         *
         * @param args action arguments
         * @return ok
         */
        // [[nodiscard]]
        // int getMessageMetadata(const std::vector<std::string> &args) const;

        /**
         * @brief Adds a tag to the topic
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int addTopicTag(const std::vector<std::string> &args) const;

        /**
         * @brief Sets the value f existing topic tag
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int setTopicTag(const std::vector<std::string> &args) const;

        /**
         * @brief Deketes a topic tag
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int deleteTopicTag(const std::vector<std::string> &args) const;

        /**
         * @brief Subscribes a target resource (e.g. an EQS queue) to a topic.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int subscribe(const std::vector<std::string> &args) const;

        /**
         * @brief Deletes a subscription.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int unsubscribe(const std::vector<std::string> &args) const;

        /**
         * @brief Lists the subscriptions of a topic.
         *
         * @param args action arguments
         * @return ok
         */
        [[nodiscard]]
        int listSubscriptions(const std::vector<std::string> &args) const;

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