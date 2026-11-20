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
     * @brief Command line for EDS, the domain name service: zones, their records, and what the
     * name server answers.
     *
     * @author jensvogt47\@gmail.com
     */
    class EdsCli final : BaseCli {

    public:
        explicit EdsCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

        /**
         * @brief Every action, with the one-line summary it is listed by - read by "help" and by
         * tab completion.
         */
        static const std::vector<std::pair<std::string, std::string> > &Actions();

        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

    private:
        [[nodiscard]] int createZone(const std::vector<std::string> &args) const;
        [[nodiscard]] int getZone(const std::vector<std::string> &args) const;
        [[nodiscard]] int listZones(const std::vector<std::string> &args) const;
        [[nodiscard]] int deleteZone(const std::vector<std::string> &args) const;
        [[nodiscard]] int putRecord(const std::vector<std::string> &args) const;
        [[nodiscard]] int deleteRecord(const std::vector<std::string> &args) const;
        [[nodiscard]] int listRecords(const std::vector<std::string> &args) const;
        [[nodiscard]] int getAnswer(const std::vector<std::string> &args) const;

        /**
         * @brief Parses args against desc, or prints why not.
         */
        [[nodiscard]] static bool parse(const std::vector<std::string> &args, const boost::program_options::options_description &desc,
                                        boost::program_options::variables_map &vm);

        /**
         * @brief Sends one request and prints the response, or the failure.
         *
         * @return the exit code.
         */
        [[nodiscard]] int call(const std::string &action, const boost::json::object &body) const;

        std::string _endpoint;
        Credentials::Entry _authentication;
        bool _pretty;
        std::string _caCertPath;
    };

}// namespace Euclid::CLI
