// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <euclid/cli/eds/EdsCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    EdsCli::EdsCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath)
        : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    const std::vector<std::pair<std::string, std::string> > &EdsCli::Actions() {
        static const std::vector<std::pair<std::string, std::string> > kActions = {
                {"create-zone", "Create a zone the name server answers for"},
                {"delete-record", "Remove the records of one type at one name"},
                {"delete-zone", "Delete a zone and every record in it"},
                {"get-answer", "Show what the name server answers for a name"},
                {"get-zone", "Show a zone, its serial and how many record sets it holds"},
                {"list-records", "List a zone's record sets"},
                {"list-zones", "List the account's zones"},
                {"put-record", "Write the records of one type at one name, replacing any there"},
        };
        return kActions;
    }

    int EdsCli::process(const std::string &action, const std::vector<std::string> &args) const {

        if (action == "help" || action == "--help" || action == "-h") return PrintModuleHelp("eds", Actions());

        if (action == "create-zone") return createZone(args);
        if (action == "get-zone") return getZone(args);
        if (action == "list-zones") return listZones(args);
        if (action == "delete-zone") return deleteZone(args);
        if (action == "put-record") return putRecord(args);
        if (action == "delete-record") return deleteRecord(args);
        if (action == "list-records") return listRecords(args);
        if (action == "get-answer") return getAnswer(args);

        std::cerr << "error: unknown EDS action '" << action << "'\n";
        return 1;
    }

    bool EdsCli::parse(const std::vector<std::string> &args, const po::options_description &desc, po::variables_map &vm) {
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
            return true;
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return false;
        }
    }

    int EdsCli::call(const std::string &action, const boost::json::object &body) const {
        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eds", action, body);
            if (!response.IsSuccess()) {
                if (response.statusCode == 401) {
                    Credentials::ClearToken();
                    std::cerr << "error: " << action << " failed: not authenticated - your session is missing, invalid or expired; run 'euclid-cli eam login' again\n";
                    return 1;
                }
                std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    // ── Zones ────────────────────────────────────────────────────────────────

    int EdsCli::createZone(const std::vector<std::string> &args) const {

        po::options_description desc("eds create-zone options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "the zone's apex, e.g. example.com")
                ("ttl", po::value<long>(), "TTL in seconds of records that name none, and of negative answers; default 300")
                ("description,d", po::value<std::string>(), "free text");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "create-zone", "--name <zone> [--ttl <seconds>] [--description <text>]",
                                   "Creates a zone, which the name server answers for from then on: with the records put "
                                   "into it, and with NXDOMAIN for every other name below it. A zone's name is unique "
                                   "across the installation, not just the account - a DNS question names no account.",
                                   desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;

        boost::json::object body{{"name", vm["name"].as<std::string>()}};
        if (vm.contains("ttl")) body["ttl"] = vm["ttl"].as<long>();
        if (vm.contains("description")) body["description"] = vm["description"].as<std::string>();
        return call("create-zone", body);
    }

    int EdsCli::getZone(const std::vector<std::string> &args) const {

        po::options_description desc("eds get-zone options");
        desc.add_options()("name,n", po::value<std::string>()->required(), "the zone's apex");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "get-zone", "--name <zone>", "Shows a zone: its TTL, its SOA serial and how many record sets it holds.", desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        return call("get-zone", {{"name", vm["name"].as<std::string>()}});
    }

    int EdsCli::listZones(const std::vector<std::string> &args) const {

        po::options_description desc("eds list-zones options");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "only zones whose name starts with this")
                ("page-size", po::value<long>(), "most to return")
                ("page-index", po::value<long>(), "zero-based page");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "list-zones", "[--prefix <text>] [--page-size <n>] [--page-index <n>]",
                                   "Lists the zones of the account and namespace you are working in.", desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;

        boost::json::object body;
        if (vm.contains("prefix")) body["prefix"] = vm["prefix"].as<std::string>();
        if (vm.contains("page-size")) body["pageSize"] = vm["page-size"].as<long>();
        if (vm.contains("page-index")) body["pageIndex"] = vm["page-index"].as<long>();
        return call("list-zones", body);
    }

    int EdsCli::deleteZone(const std::vector<std::string> &args) const {

        po::options_description desc("eds delete-zone options");
        desc.add_options()("name,n", po::value<std::string>()->required(), "the zone's apex");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "delete-zone", "--name <zone>",
                                   "Deletes a zone and every record in it. Names in it stop resolving at once; resolvers "
                                   "that cached them keep them for up to their TTL.",
                                   desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        return call("delete-zone", {{"name", vm["name"].as<std::string>()}});
    }

    // ── Records ──────────────────────────────────────────────────────────────

    int EdsCli::putRecord(const std::vector<std::string> &args) const {

        po::options_description desc("eds put-record options");
        desc.add_options()
                ("zone,z", po::value<std::string>()->required(), "the zone's apex")
                ("name,n", po::value<std::string>()->default_value("@"), "owner: relative (www), fully qualified (www.example.com.), @ for the apex, or *.dev")
                ("type,t", po::value<std::string>()->required(), "A, AAAA, CNAME, MX, NS, PTR, SRV or TXT")
                ("value,v", po::value<std::vector<std::string> >()->required()->composing(), "a value; repeat for several")
                ("ttl", po::value<long>(), "TTL in seconds; default is the zone's");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "put-record", "--zone <zone> --name <name> --type <type> --value <value> [--value <value>...] [--ttl <seconds>]",
                                   "Writes every record of one type at one name, replacing those there. Values are written "
                                   "as in a zone file: A 192.0.2.1, AAAA 2001:db8::1, CNAME host.example.com, "
                                   "MX \"10 mail.example.com\", SRV \"10 5 8080 api.example.com\", TXT any text.",
                                   desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;

        boost::json::array values;
        for (const auto &value: vm["value"].as<std::vector<std::string> >()) values.emplace_back(value);

        boost::json::object body{{"zone", vm["zone"].as<std::string>()},
                                 {"name", vm["name"].as<std::string>()},
                                 {"type", vm["type"].as<std::string>()},
                                 {"values", values}};
        if (vm.contains("ttl")) body["ttl"] = vm["ttl"].as<long>();
        return call("put-record", body);
    }

    int EdsCli::deleteRecord(const std::vector<std::string> &args) const {

        po::options_description desc("eds delete-record options");
        desc.add_options()
                ("zone,z", po::value<std::string>()->required(), "the zone's apex")
                ("name,n", po::value<std::string>()->default_value("@"), "owner name, as for put-record")
                ("type,t", po::value<std::string>()->required(), "record type");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "delete-record", "--zone <zone> --name <name> --type <type>",
                                   "Removes every record of one type at one name.", desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        return call("delete-record", {{"zone", vm["zone"].as<std::string>()},
                                      {"name", vm["name"].as<std::string>()},
                                      {"type", vm["type"].as<std::string>()}});
    }

    int EdsCli::listRecords(const std::vector<std::string> &args) const {

        po::options_description desc("eds list-records options");
        desc.add_options()
                ("zone,z", po::value<std::string>()->required(), "the zone's apex")
                ("name,n", po::value<std::string>(), "only records at this name")
                ("type,t", po::value<std::string>(), "only records of this type")
                ("page-size", po::value<long>(), "most to return")
                ("page-index", po::value<long>(), "zero-based page");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "list-records", "--zone <zone> [--name <name>] [--type <type>]",
                                   "Lists a zone's record sets, by name and type, with their values as they were put.", desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;

        boost::json::object body{{"zone", vm["zone"].as<std::string>()}};
        if (vm.contains("name")) body["name"] = vm["name"].as<std::string>();
        if (vm.contains("type")) body["type"] = vm["type"].as<std::string>();
        if (vm.contains("page-size")) body["pageSize"] = vm["page-size"].as<long>();
        if (vm.contains("page-index")) body["pageIndex"] = vm["page-index"].as<long>();
        return call("list-records", body);
    }

    int EdsCli::getAnswer(const std::vector<std::string> &args) const {

        po::options_description desc("eds get-answer options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "the name to look up")
                ("type,t", po::value<std::string>()->default_value("A"), "record type");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eds", "get-answer", "--name <name> [--type <type>]",
                                   "Shows what the name server answers for a name, from the zones it holds - the same "
                                   "answer a DNS client gets, without needing one. Names in no zone here are shown as "
                                   "REFUSED; the DNS port forwards those instead, when forwarders are configured.",
                                   desc);
        }
        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        return call("get-answer", {{"name", vm["name"].as<std::string>()}, {"type", vm["type"].as<std::string>()}});
    }

}// namespace Euclid::CLI
