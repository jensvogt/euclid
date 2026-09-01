#pragma once

// C++ includes
#include <iostream>
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/cli/BaseCli.h>
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/help/CliHelp.h>
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/JsonUtils.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the "ees" (Euclid event service) module (e.g. "ees receive-events").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EesCli final : BaseCli {

    public:

        /**
         * @brief Constructs the handler.
         *
         * @param endpoint  Euclid server endpoint
         * @param authentication authentication including the bearer token used to authenticate requests
         * @param pretty pretty print output
         * @param caCertPath if non-empty, path to a PEM CA certificate trusted in addition to the
         * system trust store, e.g. for self-signed development certificates
         */
        explicit EesCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Registers a durable subscription, or narrows an existing one.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int subscribeEvents(const std::vector<std::string> &args) const;

        /**
         * @brief Removes a subscription and the events still waiting for it.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int unsubscribeEvents(const std::vector<std::string> &args) const;

        /**
         * @brief Shows what a subscriber is subscribed to, and how many events are waiting.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listSubscriptions(const std::vector<std::string> &args) const;

        /**
         * @brief Claims a subscriber's waiting events, optionally acknowledging them at once.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int receiveEvents(const std::vector<std::string> &args) const;

        /**
         * @brief Acknowledges claimed events, which is what deletes them.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int ackEvents(const std::vector<std::string> &args) const;

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
