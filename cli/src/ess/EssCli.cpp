// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <euclid/cli/ess/EssCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    EssCli::EssCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath)
        : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EssCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ess", {
                                           {"create-secret", "Store a new secret"},
                                           {"delete-secret", "Delete a secret"},
                                           {"get-secret", "Read a secret's value"},
                                           {"list-secrets", "List stored secrets, without their values"},
                                           {"update-secret", "Rotate a secret, or change its description or key"},
                                   });
        }
        if (action == "create-secret") {
            return createSecret(args);
        }
        if (action == "get-secret") {
            return getSecret(args);
        }
        if (action == "list-secrets") {
            return listSecrets(args);
        }
        if (action == "update-secret") {
            return updateSecret(args);
        }
        if (action == "delete-secret") {
            return deleteSecret(args);
        }
        std::cerr << "error: unknown ESS action '" << action << "'\n";
        return 1;
    }

    bool EssCli::readValue(const po::variables_map &vm, std::string &value) {

        // The value of the switch, not its presence: a bool_switch is in the map whether or not
        // it was given, so asking whether it is there counts a source nobody named.
        const bool fromStdin = vm.contains("value-stdin") && vm["value-stdin"].as<bool>();

        const auto sources = static_cast<int>(vm.contains("value")) + static_cast<int>(vm.contains("value-file")) + static_cast<int>(fromStdin);
        if (sources > 1) {
            std::cerr << "error: give the value once - --value, --value-file or --value-stdin\n";
            return false;
        }

        if (vm.contains("value")) {
            value = vm["value"].as<std::string>();
            return true;
        }

        if (vm.contains("value-file")) {
            const auto path = vm["value-file"].as<std::string>();
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                std::cerr << "error: could not open '" << path << "'\n";
                return false;
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            value = buffer.str();

            // A file written by an editor ends in a newline that was never part of the password;
            // one trailing newline goes, anything else the caller meant is kept.
            if (!value.empty() && value.back() == '\n') value.pop_back();
            if (!value.empty() && value.back() == '\r') value.pop_back();
            return true;
        }

        if (fromStdin) {
            std::ostringstream buffer;
            buffer << std::cin.rdbuf();
            value = buffer.str();
            if (!value.empty() && value.back() == '\n') value.pop_back();
            if (!value.empty() && value.back() == '\r') value.pop_back();
            return true;
        }

        std::cerr << "error: no value given - use --value, --value-file or --value-stdin\n";
        return false;
    }

    int EssCli::createSecret(const std::vector<std::string> &args) const {
        po::options_description desc("store a new secret");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name the secret is stored under, and the one an application asks for")
                ("value,v", po::value<std::string>(), "the secret itself; visible in the process list and the shell history, so prefer the two below")
                ("value-file,f", po::value<std::string>(), "read the value from a file, dropping one trailing newline")
                ("value-stdin", po::bool_switch(), "read the value from standard input")
                ("description,d", po::value<std::string>(), "what the secret is for; readable by anyone who may list secrets")
                ("key,k", po::value<std::string>(), "ERN of the EKM key to encrypt it under (default: the namespace's own secrets key)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ess", "create-secret", "--name <name> (--value <text> | --value-file <file> | --value-stdin) [--description <text>] [--key <ern>]",
                                   "Stores a secret - a password, a connection string, a token - encrypted under an EKM key. "
                                   "The value is encrypted before it is written and decrypted only by \"ess get-secret\", so a "
                                   "copy of the database yields nothing without the key management module. "
                                   "Without --key the secret is encrypted under the namespace's own secrets key, which ESS "
                                   "creates the first time something needs it; naming a key puts this secret under one you "
                                   "manage yourself. "
                                   "Prefer --value-file or --value-stdin: a value passed as --value is visible in the process "
                                   "list while the command runs and stays in the shell history afterwards. "
                                   "The response carries the secret's metadata and never its value.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESS::CreateSecretRequest request;
        request.name = vm["name"].as<std::string>();
        if (vm.contains("description")) request.description = vm["description"].as<std::string>();
        if (vm.contains("key")) request.keyErn = vm["key"].as<std::string>();
        if (!readValue(vm, request.value)) return 1;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ess", "create-secret", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: create-secret failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EssCli::getSecret(const std::vector<std::string> &args) const {
        po::options_description desc("read a secret's value");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "secret name")
                ("raw,r", po::bool_switch(), "print the value alone, with no JSON and no trailing newline");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ess", "get-secret", "--name <name> [--raw]",
                                   "Reads a secret and prints it. This is the only action in euclid that writes a secret's "
                                   "value out, and the server records every one of these in its log - who read what and when, "
                                   "never the value itself. "
                                   "--raw prints the value alone, with no JSON around it and no trailing newline, which is what "
                                   "makes it usable from a script: PGPASSWORD=$(euclid-cli ess get-secret -n db --raw). "
                                   "Without it the metadata comes too, which is what says how old the value is and which key it "
                                   "is encrypted under.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESS::SecretNameRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ess", "get-secret", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-secret failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }

            if (vm["raw"].as<bool>()) {
                // No newline: what is substituted into a variable or piped into another program
                // should be the value and nothing else.
                std::cout << Core::GetStringValue(response.body, "value") << std::flush;
                return 0;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EssCli::listSecrets(const std::vector<std::string> &args) const {
        po::options_description desc("list the stored secrets");
        desc.add_options()
                ("prefix,p", po::value<std::string>()->default_value(""), "only secrets whose name starts with this")
                ("page-size,s", po::value<long>()->default_value(10), "page size")
                ("page-index,i", po::value<long>()->default_value(0), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column (name, ern, rotated)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ess", "list-secrets", "[--prefix <text>] [--page-size <n>] [--page-index <n>] [--sort-column <column>]",
                                   "Lists the secrets of the caller's account and namespace with their descriptions, versions, "
                                   "the key each is encrypted under and when each was last rotated - but never their values. "
                                   "Sorting by \"rotated\" puts the stalest first, which is what answers which passwords nobody "
                                   "has changed since they were written down. "
                                   "A principal deployed with particular secrets sees exactly those.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESS::ListSecretsRequest request;
        request.prefix = vm["prefix"].as<std::string>();
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ess", "list-secrets", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-secrets failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EssCli::updateSecret(const std::vector<std::string> &args) const {
        po::options_description desc("rotate a secret or change what is recorded about it");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "secret name")
                ("value,v", po::value<std::string>(), "the new value; visible in the process list and the shell history, so prefer the two below")
                ("value-file,f", po::value<std::string>(), "read the new value from a file, dropping one trailing newline")
                ("value-stdin", po::bool_switch(), "read the new value from standard input")
                ("description,d", po::value<std::string>(), "what the secret is for; an empty string clears it")
                ("key,k", po::value<std::string>(), "ERN of another EKM key to encrypt it under from now on");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ess", "update-secret", "--name <name> [--value <text> | --value-file <file> | --value-stdin] [--description <text>] [--key <ern>]",
                                   "Changes a secret that already exists: its value, its description, or the key it is "
                                   "encrypted under. Only what is named changes, so a rotation does not have to restate the "
                                   "description and a change of description does not touch the value. "
                                   "Setting a value is a rotation: the version moves on, the previous value is gone, and an "
                                   "\"ess.secret.rotated\" event is published so anything holding the old one can reload it. "
                                   "Naming --key re-encrypts the secret under another key without changing the value or the "
                                   "version, which is how a secret is moved off a key that is being retired.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESS::UpdateSecretRequest request;
        request.name = vm["name"].as<std::string>();
        if (vm.contains("description")) {
            request.hasDescription = true;
            request.description = vm["description"].as<std::string>();
        }
        if (vm.contains("key")) request.keyErn = vm["key"].as<std::string>();

        if (vm.contains("value") || vm.contains("value-file") || vm["value-stdin"].as<bool>()) {
            request.hasValue = true;
            if (!readValue(vm, request.value)) return 1;
        }

        if (!request.hasValue && !request.hasDescription && request.keyErn.empty()) {
            std::cerr << "error: nothing to change - give --value, --description or --key\n";
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ess", "update-secret", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: update-secret failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EssCli::deleteSecret(const std::vector<std::string> &args) const {
        po::options_description desc("delete a secret");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "secret name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ess", "delete-secret", "--name <name>",
                                   "Deletes a secret and the value it holds, immediately and for good. Unlike deleting an EKM "
                                   "key, there is no grace period: a secret is the thing itself rather than what protects other "
                                   "things, so there would be nothing left to recover afterwards and nothing a delay could save. "
                                   "Whatever was using the secret will fail to read it on its next attempt.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ESS::SecretNameRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ess", "delete-secret", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-secret failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

}// namespace Euclid::CLI
