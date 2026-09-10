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
#include <euclid/cli/http/HttpClient.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/ekv/CreateTableRequest.h>
#include <euclid/dto/ekv/DeleteItemRequest.h>
#include <euclid/dto/ekv/DeleteTableRequest.h>
#include <euclid/dto/ekv/DescribeTableRequest.h>
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
     * @author jens.vogt\@opitz-consulting.com
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
         * @brief Dispatches to the handler for the given action. Returns the process exit code.
         */
        [[nodiscard]]
        int process(const std::string &action, const std::vector<std::string> &args) const;

    private:

        [[nodiscard]] int createTable(const std::vector<std::string> &args) const;
        [[nodiscard]] int describeTable(const std::vector<std::string> &args) const;
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
