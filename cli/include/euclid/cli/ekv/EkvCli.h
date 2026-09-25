// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/json.hpp>
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/cli/BaseCli.h>
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/ExistsCheck.h>
#include <euclid/cli/help/CliHelp.h>
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/ekv/CreateTableRequest.h>
#include <euclid/dto/ekv/DeleteItemRequest.h>
#include <euclid/dto/ekv/DeleteTableRequest.h>
#include <euclid/dto/ekv/GetTableRequest.h>
#include <euclid/dto/ekv/GetItemRequest.h>
#include <euclid/dto/ekv/ListTablesRequest.h>
#include <euclid/dto/ekv/PutItemRequest.h>
#include <euclid/dto/ekv/QueryRequest.h>
#include <euclid/dto/ekv/ScanRequest.h>

namespace Euclid::CLI {

    /**
     * @brief Processes commands for the ekv (Euclid Key/Value store) module
     * (e.g. "ekv get-item --table suppliers --key '{\"supplierId\":\"4711\"}'").
     *
     * @author jensvogt47\@gmail.com
     */
    class EkvCli final : BaseCli {

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
        explicit EkvCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

        /**
         * @brief Every action this module takes, with the one-line summary each is listed by.
         *
         * @return the actions, in the order "help" lists them
         */
        static const std::vector<std::pair<std::string, std::string> > &Actions();

        /**
         * @brief Dispatches to the handler for the given action. Returns the process exit code.
         */
        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

        /**
         * @brief Whether a table exists, answered as an exit code a shell can branch on.
         *
         * @par
         * Written for `if euclid-cli ekv exists-table -n name; then`. 0 when it is there, 1 when it
         * is not, 2 when the question could not be answered - see Exists for why the third code
         * matters more than it looks. "true" or "false" goes to stdout and nothing else does.
         *
         * @param args command line arguments
         * @return 0 if the table exists, 1 if it does not, 2 if the question could not be answered
         */
        [[nodiscard]]
        int existsTable(const std::vector<std::string> &args) const;

    private:

        [[nodiscard]] int createTable(const std::vector<std::string> &args) const;
        [[nodiscard]] int getTable(const std::vector<std::string> &args) const;
        [[nodiscard]] int listTables(const std::vector<std::string> &args) const;
        [[nodiscard]] int deleteTable(const std::vector<std::string> &args) const;
        [[nodiscard]] int putItem(const std::vector<std::string> &args) const;
        [[nodiscard]] int getItem(const std::vector<std::string> &args) const;
        [[nodiscard]] int deleteItem(const std::vector<std::string> &args) const;
        [[nodiscard]] int query(const std::vector<std::string> &args) const;
        [[nodiscard]] int scan(const std::vector<std::string> &args) const;

        /**
         * @brief Reads a JSON document from an option, or from the file an option names.
         *
         * @par
         * A supplier record is not something anybody wants to quote into a shell twice, so
         * anything that takes a document takes a file instead.
         */
        static bool readJson(const boost::program_options::variables_map &vm, const std::string &option,
                             boost::json::value &value, std::string &error);

        /**
         * @brief The euclid server endpoint.
         */
        std::string _endpoint;

        /**
         * @brief Authentication, including the bearer token.
         */
        Credentials::Entry _authentication;

        /**
         * @brief Whether to pretty-print the output.
         */
        bool _pretty;

        /**
         * @brief Extra CA certificate to trust, if any.
         */
        std::string _caCertPath;
    };

}// namespace Euclid::CLI
