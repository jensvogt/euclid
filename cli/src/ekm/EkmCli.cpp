// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <euclid/cli/ekm/EkmCli.h>

#include "euclid/dto/ekm/ListKeysRequest.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace Euclid::CLI {

    namespace po = boost::program_options;

    EkmCli::EkmCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EkmCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ekm", {
                                           {"create-certificate", "Generate and store a self-signed certificate"},
                                           {"create-key", "Create a new key"},
                                           {"decrypt", "Decrypt a file or stdin with a key"},
                                           {"delete-certificate", "Delete a certificate"},
                                           {"delete-key", "Schedule a key for deletion"},
                                           {"encrypt", "Encrypt a file or stdin with a key"},
                                           {"get-certificate", "Show one certificate, without its private key"},
                                           {"import-certificate", "Store a certificate and its private key"},
                                           {"list-certificates", "List stored certificates"},
                                           {"list-keys", "List existing keys"},
                                           {"revoke-key", "Revoke a key (blocks encryption, decryption still works)"},
                                           {"set-key-description", "Change what a key says it is for"},
                                   });
        }
        if (action == "create-key") {
            return createKey(args);
        }
        if (action == "list-keys") {
            return listKeys(args);
        }
        if (action == "delete-key") {
            return deleteKey(args);
        }
        if (action == "revoke-key") {
            return revokeKey(args);
        }
        if (action == "set-key-description") {
            return setKeyDescription(args);
        }
        if (action == "encrypt") {
            return encrypt(args);
        }
        if (action == "decrypt") {
            return decrypt(args);
        }
        if (action == "import-certificate") {
            return importCertificate(args);
        }
        if (action == "create-certificate") {
            return createCertificate(args);
        }
        if (action == "list-certificates") {
            return listCertificates(args);
        }
        if (action == "get-certificate") {
            return getCertificate(args);
        }
        if (action == "delete-certificate") {
            return deleteCertificate(args);
        }
        std::cerr << "error: unknown EKM action '" << action << "'\n";
        return 1;
    }

    int EkmCli::createKey(const std::vector<std::string> &args) const {
        po::options_description desc("create a new key");
        desc.add_options()
                ("algorithm,a", po::value<std::string>()->required(), "key type, possible values are aes, aes-gcm")
                ("length,l", po::value<long>()->default_value(128), "key length (128 or 256, default:128)")
                ("description,d", po::value<std::string>(), "what the key is for, kept with it and shown by list-keys");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "create-key", "--algorithm <algorithm> [--length <length>] [--description <text>]",
                                   "Creates a new key with the given algorithm and the key length. Default length is 128bit. "
                                   "A key is identified by a generated ID, which says nothing about what it protects - so "
                                   "--description is where to record that. It is free text, stored with the key and reported by "
                                   "list-keys, and it is what tells somebody months later whether a key can be deleted. Deleting "
                                   "the wrong one is not a recoverable mistake.",
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

        Dto::EKM::CreateKeyRequest request;
        request.algorithm = vm["algorithm"].as<std::string>();
        request.length = vm["length"].as<long>();
        if (vm.contains("description")) request.description = vm["description"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "create-key", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: create-key failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::listKeys(const std::vector<std::string> &args) const {
        po::options_description desc("lists all keys");
        desc.add_options()
                ("page-size,s", po::value<long>()->default_value(10), "page size")
                ("page-index,i", po::value<long>()->default_value(0), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "list-keys", "[--page-size <n>] [--page-index <n>] [--sort-column <column>]",
                                   "Lists all keys. Paginated: pageSize defaults to 10, pageIndex to 0, sortColumn to \"name\".",
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

        Dto::EKM::ListKeysRequest request;
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "list-keys", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-keys failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::deleteKey(const std::vector<std::string> &args) const {
        po::options_description desc("schedule a key for deletion");
        desc.add_options()
                ("key-id,k", po::value<std::string>()->required(), "ID of the key to delete (as returned by create-key)")
                ("pending-window-days,d", po::value<long>()->default_value(7), "days to wait before the key is permanently deleted");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "delete-key", "--key-id <id> [--pending-window-days <days>]",
                                   "Schedules a key for permanent deletion after a grace period (default 7 days). "
                                   "The key can still be used to decrypt during that time - so there is time to "
                                   "decrypt or migrate everything encrypted under it before it becomes unrecoverable "
                                   "- but it can no longer be used to encrypt.",
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

        Dto::EKM::DeleteKeyRequest request;
        request.keyId = vm["key-id"].as<std::string>();
        request.pendingWindowInDays = vm["pending-window-days"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "delete-key", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-key failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::revokeKey(const std::vector<std::string> &args) const {
        po::options_description desc("revoke a key");
        desc.add_options()
                ("key,k", po::value<std::string>()->required(), "ERN of the key to revoke");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "revoke-key", "--key <ern>",
                                   "Revokes a key: it can no longer be used to encrypt, but decryption of data "
                                   "already encrypted under it keeps working. Unlike delete-key, revoking does not "
                                   "schedule the key for removal.",
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

        Dto::EKM::RevokeKeyRequest request;
        request.ern = vm["key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "revoke-key", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: revoke-key failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::setKeyDescription(const std::vector<std::string> &args) const {
        po::options_description desc("set a key's description");
        desc.add_options()
                ("key,k", po::value<std::string>()->required(), "ERN of the key to describe")
                ("description,d", po::value<std::string>()->required(), "what the key is for; an empty string clears it");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "set-key-description", "--key <ern> --description <text>",
                                   "Changes what a key says it is for. A key is identified by a generated ID that says "
                                   "nothing about what it protects, so the description is what answers - months later, "
                                   "when somebody is deciding whether the key can be deleted - what would be lost. "
                                   "create-key takes one; this is how it is corrected afterwards, and it is worth "
                                   "correcting: a key that has been revoked or scheduled for deletion is exactly the "
                                   "kind whose description should say why. "
                                   "Passing an empty string clears the description rather than leaving it alone, since "
                                   "otherwise there would be no way to remove one. "
                                   "The key is named by ERN, as revoke-key names it; list-keys prints both the ERN and "
                                   "the ID.",
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

        Dto::EKM::SetKeyDescriptionRequest request;
        request.ern = vm["key"].as<std::string>();
        request.description = vm["description"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "set-key-description", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: set-key-description failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    bool EkmCli::readPemFile(const std::string &path, std::string &contents) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "error: could not open '" << path << "'\n";
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        contents = buffer.str();
        if (contents.empty()) {
            std::cerr << "error: '" << path << "' is empty\n";
            return false;
        }
        return true;
    }

    int EkmCli::importCertificate(const std::vector<std::string> &args) const {
        po::options_description desc("store a certificate and its private key");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name the certificate is stored under, and the one a listener names")
                ("certificate,f", po::value<std::string>()->required(), "path to the PEM certificate file, leaf first, intermediates after it")
                ("key,k", po::value<std::string>()->required(), "path to the PEM private key file, unencrypted")
                ("description,d", po::value<std::string>(), "what the certificate is for, kept with it and shown by list-certificates");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "import-certificate", "--name <name> --certificate <file> --key <file> [--description <text>]",
                                   "Stores a certificate somebody else issued, together with its private key, under a name. "
                                   "An API gateway listener configured with \"protocol\": \"https\" serves the certificate its "
                                   "\"certificate\" setting names, so importing under that name is how a real certificate replaces "
                                   "the self-signed one euclid generated for it - and how a renewed certificate is rolled out, "
                                   "since importing again under the same name replaces what is there. "
                                   "The key is checked against the certificate before either is stored: a mismatched pair would "
                                   "otherwise be accepted here and only show itself as a handshake that fails for every caller. "
                                   "The private key never leaves the server again - no action reports it.",
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

        Dto::EKM::ImportCertificateRequest request;
        request.name = vm["name"].as<std::string>();
        if (vm.contains("description")) request.description = vm["description"].as<std::string>();
        if (!readPemFile(vm["certificate"].as<std::string>(), request.certificate)) return 1;
        if (!readPemFile(vm["key"].as<std::string>(), request.privateKey)) return 1;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "import-certificate", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: import-certificate failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::createCertificate(const std::vector<std::string> &args) const {
        po::options_description desc("generate and store a self-signed certificate");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name the certificate is stored under")
                ("common-name,c", po::value<std::string>(), "subject common name, normally the host name callers use (default: the certificate name)")
                ("alt-name,a", po::value<std::vector<std::string> >()->multitoken(), "further host name or IP address the certificate is valid for; may be given more than once")
                ("valid-days,v", po::value<long>()->default_value(825), "how many days the certificate is valid for")
                ("key-bits,b", po::value<long>()->default_value(2048), "RSA key length in bits")
                ("description,d", po::value<std::string>(), "what the certificate is for");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "create-certificate", "--name <name> [--common-name <host>] [--alt-name <host>]... [--valid-days <n>] [--key-bits <n>] [--description <text>]",
                                   "Generates a self-signed certificate and stores it under a name, for an installation that has "
                                   "to serve HTTPS before anybody has bought it a real certificate - development, a demonstration, "
                                   "an internal network where the clients can be told what to trust. "
                                   "Nobody has vouched for the result: a client rejects it until it is given the certificate as "
                                   "something to trust, which is what --ca-cert does for euclid-cli itself. "
                                   "Give every name callers will use as --common-name or --alt-name; a client checks the name it "
                                   "dialled against these and refuses the connection if it is not among them.",
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

        Dto::EKM::CreateCertificateRequest request;
        request.name = vm["name"].as<std::string>();
        if (vm.contains("common-name")) request.commonName = vm["common-name"].as<std::string>();
        if (vm.contains("alt-name")) request.subjectAltNames = vm["alt-name"].as<std::vector<std::string> >();
        if (vm.contains("description")) request.description = vm["description"].as<std::string>();
        request.validDays = vm["valid-days"].as<long>();
        request.keyBits = vm["key-bits"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "create-certificate", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: create-certificate failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::listCertificates(const std::vector<std::string> &args) const {
        po::options_description desc("lists the stored certificates");
        desc.add_options()
                ("prefix,p", po::value<std::string>()->default_value(""), "only certificates whose name starts with this")
                ("page-size,s", po::value<long>()->default_value(10), "page size")
                ("page-index,i", po::value<long>()->default_value(0), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column (name, ern, notAfter)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "list-certificates", "[--prefix <text>] [--page-size <n>] [--page-index <n>] [--sort-column <column>]",
                                   "Lists the stored certificates with their subject, issuer, fingerprint and validity period, "
                                   "but never their private keys. Sorting by \"notAfter\" puts the next one to expire first, "
                                   "which is what answers what is about to stop working.",
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

        Dto::EKM::ListCertificatesRequest request;
        request.prefix = vm["prefix"].as<std::string>();
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "list-certificates", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-certificates failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::getCertificate(const std::vector<std::string> &args) const {
        po::options_description desc("show one certificate");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "certificate name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "get-certificate", "--name <name>",
                                   "Shows one certificate: its subject, issuer, fingerprint, validity period and the PEM itself, "
                                   "which is what somebody needs in order to add a self-signed certificate to a trust store. "
                                   "The private key is not reported - it never leaves the server.",
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

        Dto::EKM::CertificateNameRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "get-certificate", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-certificate failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::deleteCertificate(const std::vector<std::string> &args) const {
        po::options_description desc("delete a certificate");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "certificate name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "delete-certificate", "--name <name>",
                                   "Deletes a certificate and its private key, immediately and without a grace period: unlike a "
                                   "key, nothing becomes unreadable. A gateway listener already serving it keeps the copy it "
                                   "loaded until it is restarted, and generates a new self-signed certificate if it restarts "
                                   "and finds none - so a listener is never left with nothing to answer with.",
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

        Dto::EKM::CertificateNameRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekm", "delete-certificate", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-certificate failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkmCli::encrypt(const std::vector<std::string> &args) const {
        return runTransform("encrypt", "encrypt a file or stdin with a key",
                            "Encrypts a file or stdin with the given key and writes the ciphertext to a file or stdout.", args);
    }

    int EkmCli::decrypt(const std::vector<std::string> &args) const {
        return runTransform("decrypt", "decrypt a file or stdin with a key",
                            "Decrypts a file or stdin with the given key and writes the plaintext to a file or stdout.", args);
    }

    int EkmCli::runTransform(const std::string &action, const std::string &caption, const std::string &description, const std::vector<std::string> &args) const {
        po::options_description desc(caption);
        desc.add_options()
                ("key-id,k", po::value<std::string>()->required(), "ID of the key to use (as returned by create-key)")
                ("input,i", po::value<std::string>(), "input file path (default: stdin)")
                ("output,o", po::value<std::string>(), "output file path (default: stdout)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", action, "--key-id <id> [--input <file>] [--output <file>]", description, desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        std::string data;
        if (vm.count("input")) {
            std::ifstream in(vm["input"].as<std::string>(), std::ios::binary);
            if (!in.is_open()) {
                std::cerr << "error: could not open input file '" << vm["input"].as<std::string>() << "'\n";
                return 1;
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            data = buffer.str();
        } else {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
#endif
            std::ostringstream buffer;
            buffer << std::cin.rdbuf();
            data = buffer.str();
        }

        const std::vector<std::pair<std::string, std::string> > headers{
                {"x-euclid-key-id", vm["key-id"].as<std::string>()},
        };

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const BinaryHttpResponse response = client.PostBinaryForBinary("ekm", action, headers, data);
            if (!response.IsSuccess()) {
                std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.errorBody) << std::endl;
                return 1;
            }

            if (vm.count("output")) {
                std::ofstream out(vm["output"].as<std::string>(), std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    std::cerr << "error: could not open output file '" << vm["output"].as<std::string>() << "'\n";
                    return 1;
                }
                out.write(response.data.data(), static_cast<std::streamsize>(response.data.size()));
            } else {
#ifdef _WIN32
                _setmode(_fileno(stdout), _O_BINARY);
#endif
                std::cout.write(response.data.data(), static_cast<std::streamsize>(response.data.size()));
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

}// namespace Euclid::CLI