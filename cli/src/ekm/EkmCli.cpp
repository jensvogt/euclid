#include <euclid/cli/ekm/EkmCli.h>

#include "euclid/dto/ekm/ListKeysRequest.h"

namespace Euclid::CLI {

    namespace po = boost::program_options;

    EkmCli::EkmCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EkmCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ekm", {
                                           {"create-key", "Create a new key"},
                                           {"list-keys", "List existing keys"},
                                   });
        }
        if (action == "create-key") {
            return createKey(args);
        }
        if (action == "list-keys") {
            return listKeys(args);
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
            const HttpResponse response = client.Post("eqs", "create-key", boost::json::value_from(request));
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
            const HttpResponse response = client.Post("eqs", "list-keys", boost::json::value_from(request));
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

}// namespace Euclid::CLI