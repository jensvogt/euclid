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
                                           {"create-key", "Create a new key"},
                                           {"list-keys", "List existing keys"},
                                           {"encrypt", "Encrypt a file or stdin with a key"},
                                           {"decrypt", "Decrypt a file or stdin with a key"},
                                   });
        }
        if (action == "create-key") {
            return createKey(args);
        }
        if (action == "list-keys") {
            return listKeys(args);
        }
        if (action == "encrypt") {
            return encrypt(args);
        }
        if (action == "decrypt") {
            return decrypt(args);
        }
        std::cerr << "error: unknown EKM action '" << action << "'\n";
        return 1;
    }

    int EkmCli::createKey(const std::vector<std::string> &args) const {
        po::options_description desc("create a new key");
        desc.add_options()
                ("algorithm,a", po::value<std::string>()->required(), "key type, possible values are aes, aes-gcm")
                ("length,l", po::value<long>()->default_value(128), "key length (128 or 256, default:128)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekm", "create-key", "--algorithm <algorithm> [--length <length>]",
                                   "Creates a new key with the given algorithm and the key length. Default length is 128bit",
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