// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Euclid includes
#include <euclid/cli/ead/EadCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    EadCli::EadCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EadCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ead", {
                                           {"count-events", "Count audit entries matching a filter"},
                                           {"list-events", "Show the audit trail, newest first"},
                                           {"purge-events", "Remove audit entries older than a given age"},
                                   });
        }

        if (!IsHelpRequest(args) && _authentication.token.empty()) {
            std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
            return 1;
        }

        if (action == "list-events") return listEvents(args);
        if (action == "count-events") return countEvents(args);
        if (action == "purge-events") return purgeEvents(args);

        std::cerr << "error: unknown ead action '" << action << "'\n";
        return 1;
    }

    namespace {

        // The three narrowings every read here shares, added only when given so that "no filter"
        // is no filter rather than a match on an empty string.
        void addFilters(boost::json::object &request, const po::variables_map &vm) {
            if (vm.contains("user")) request["userId"] = vm["user"].as<std::string>();
            if (vm.contains("module")) request["module"] = vm["module"].as<std::string>();
            if (vm.contains("command")) request["command"] = vm["command"].as<std::string>();
        }

        void addFilterOptions(po::options_description &desc) {
            desc.add_options()
                    ("user,u", po::value<std::string>(), "only this user")
                    ("module,m", po::value<std::string>(), "only this module, e.g. esm")
                    ("command,c", po::value<std::string>(), "only this command, e.g. delete-bucket");
        }

    }// namespace

    int EadCli::listEvents(const std::vector<std::string> &args) const {

        po::options_description desc("list audit events");
        addFilterOptions(desc);
        desc.add_options()
                ("page-size,s", po::value<long>()->default_value(10), "page size")
                ("page-index,i", po::value<long>()->default_value(0), "page index");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ead", "list-events",
                                   "[--user <id>] [--module <name>] [--command <name>] [--page-size <n>] [--page-index <n>]",
                                   "Shows the audit trail, newest first - what was run, by whom, in which account and "
                                   "namespace, with the parameters it carried and the status it was answered with. "
                                   "Confined to the caller's own account whatever the filter says. "
                                   "What is in it: everything that changed something, and everything that was refused "
                                   "or failed whatever its kind - a delete somebody was not allowed to make is the "
                                   "entry this exists for, and it is a 403 that otherwise looks exactly like a delete "
                                   "nobody attempted. Successful reads are left out unless the installation sets "
                                   "euclid.modules.ead.audit-reads, because on a working system they outnumber "
                                   "everything else by orders of magnitude. "
                                   "Parameters have their sensitive values replaced with *** before they are ever "
                                   "stored, so a password is never in the trail to be read here.",
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

        boost::json::object request{{"pageSize", vm["page-size"].as<long>()}, {"pageIndex", vm["page-index"].as<long>()}};
        addFilters(request, vm);

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ead", "list-events", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: list-events failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EadCli::countEvents(const std::vector<std::string> &args) const {

        po::options_description desc("count audit events");
        addFilterOptions(desc);

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ead", "count-events", "[--user <id>] [--module <name>] [--command <name>]",
                                   "Counts audit entries without listing them, within the caller's own account. The "
                                   "same filters list-events takes.",
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

        boost::json::object request;
        addFilters(request, vm);

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ead", "count-events", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: count-events failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EadCli::purgeEvents(const std::vector<std::string> &args) const {

        po::options_description desc("purge audit events");
        desc.add_options()
                ("older-than-days,d", po::value<long>()->required(), "remove entries older than this many days");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ead", "purge-events", "--older-than-days <n>",
                                   "Removes audit entries older than the given age. Retention is normally the "
                                   "database's own job - euclid.modules.ead.retention puts an expiry on each entry as "
                                   "it is written - so this is for the installation that changed its mind and needs a "
                                   "window gone now. "
                                   "An age is required and must be positive: a purge with no window would remove the "
                                   "whole trail, and that is not something to do by leaving an argument out. The purge "
                                   "is itself audited.",
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

        const boost::json::object request{{"olderThanDays", vm["older-than-days"].as<long>()}};

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ead", "purge-events", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: purge-events failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
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
