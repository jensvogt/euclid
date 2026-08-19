#include <euclid/cli/storage/StorageCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        /**
         * @brief Resolves a value that may be given literally or as "file://path", in which case it is read from the referenced file instead.
         */
        std::string ResolveFileOrLiteral(const std::string &value) {
            constexpr std::string_view prefix = "file://";
            if (!value.starts_with(prefix)) {
                return value;
            }
            const std::string path = value.substr(prefix.size());
            std::ifstream in(path);
            if (!in.is_open()) {
                throw std::runtime_error("could not open file '" + path + "'");
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

    }

    StorageCli::StorageCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int StorageCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("queues", {
                                           {"create-bucket", "Create a new bucket"},
                                   });
        }
        if (action == "create-bucket") {
            return createBucket(args);
        }
        std::cerr << "error: unknown SQS action '" << action << "'\n";
        return 1;
    }

    int StorageCli::createBucket(const std::vector<std::string> &args) const {
        po::options_description desc("create bucket options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("storage", "create-bucket", "--name <name>",
                                   "Creates a new storage bucket with the given name.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        Dto::Storage::CreateBucketRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("storage", "create-bucket", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: create-bucket failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

}