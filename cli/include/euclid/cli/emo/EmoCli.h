// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <iostream>
#include <string>
#include <utility>
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
     * @brief Processes commands for the "emo" (Euclid monitoring) module - its threshold alerting
     * (e.g. "emo create-alert-rule", "emo list-alerts").
     *
     * @author jensvogt47\@gmail.com
     */
    class EmoCli final : BaseCli {

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
        explicit EmoCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

        /**
         * @brief Every action this module takes, with the one-line summary each is listed by.
         *
         * @return the actions, in the order "help" lists them
         */
        static const std::vector<std::pair<std::string, std::string> > &Actions();

        /**
         * @brief Dispatches to the handler for the given action. Returns the process exit code.
         *
         * @param action action string
         * @param args list of action arguments
         */
        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

      private:

        [[nodiscard]]
        int listMetrics(const std::vector<std::string> &args) const;

        [[nodiscard]]
        int createAlertRule(const std::vector<std::string> &args) const;

        [[nodiscard]]
        int updateAlertRule(const std::vector<std::string> &args) const;

        [[nodiscard]]
        int deleteAlertRule(const std::vector<std::string> &args) const;

        [[nodiscard]]
        int listAlertRules(const std::vector<std::string> &args) const;

        [[nodiscard]]
        int listAlerts(const std::vector<std::string> &args) const;

        [[nodiscard]]
        int listAlertHistory(const std::vector<std::string> &args) const;

        /**
         * @brief Sends one request and prints the answer. Returns the process exit code.
         *
         * @param action action to call
         * @param request its body
         */
        [[nodiscard]]
        int send(const std::string &action, const boost::json::object &request) const;

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

}// namespace Euclid::CLI
