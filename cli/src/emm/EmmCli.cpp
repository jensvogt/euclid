// Euclid includes
#include <euclid/cli/emm/EmmCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    EmmCli::EmmCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EmmCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("emm", {
                                           {"list-modules", "List modules known to the manager"},
                                   });
        }

        // Every emm action is administrator-only. Checked here (once, ahead of dispatch) rather
        // than per-action, so a new action added later doesn't need to remember this itself. This
        // is a client-side fail-fast for a better error message only, not the real security
        // boundary - the server independently re-checks admin privileges on every request and
        // would reject it regardless of what a modified/older CLI binary sends.
        if (_authentication.token.empty()) {
            std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
            return 1;
        }
        if (!_authentication.isAdmin) {
            std::cerr << "error: " << action << " failed: administrator privileges required\n";
            return 1;
        }

        if (action == "list-modules") {
            return listModules(args);
        }
        std::cerr << "error: unknown emm action '" << action << "'\n";
        return 1;
    }

    int EmmCli::listModules(const std::vector<std::string> &args) const {
        const po::options_description desc("list modules options");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "list-modules", "",
                                   "Lists modules known to the manager, as persisted in the module repository.",
                                   desc);
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emm", "list-modules", boost::json::object{});
            if (!response.IsSuccess()) {
                std::cerr << "error: list-modules failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
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
