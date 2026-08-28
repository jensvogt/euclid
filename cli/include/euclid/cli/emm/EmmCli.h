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
     * @brief Processes commands for the "emm" (Euclid module manager) module (e.g. "emm list-modules").
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EmmCli final : BaseCli {

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
        explicit EmmCli(std::string endpoint, Credentials::Entry authentication = {}, bool pretty = true, std::string caCertPath = {});

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
         * @brief Lists modules known to the manager, as persisted in the module repository
         * (Mongo or in-memory, per euclid.database.backend).
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int listModules(const std::vector<std::string> &args) const;

        /**
         * @brief Exports one or more modules' own MongoDB collections to a local JSON file.
         * --module takes a comma-separated list; --all exports every module instead - exactly one
         * of the two is required. By default only each module's top-level resource collection(s)
         * are included (what euclid-cli's own list-* actions show: queues, topics, buckets, ...);
         * --full additionally includes the bulk child data each resource owns (EQS/ENS messages,
         * ESM objects).
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int exportModule(const std::vector<std::string> &args) const;

        /**
         * @brief Imports a JSON file previously written by "emm export" back into MongoDB,
         * upserting each document by "_id" (replacing the whole document, not merging fields).
         * --module optionally restricts the import to a comma-separated subset of the modules
         * present in the file; by default everything the file contains is imported.
         *
         * @param args command line arguments
         * @return ok
         */
        [[nodiscard]]
        int importModule(const std::vector<std::string> &args) const;

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