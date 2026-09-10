#include <euclid/cli/ekv/EkvCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Reports a failed response, the way every other module's CLI does.
        void reportFailure(const std::string_view action, const HttpResponse &response) {
            if (response.statusCode == 401) {
                Credentials::ClearToken();
                std::cerr << "error: " << action << " failed: not authenticated - your session is missing, invalid or expired; run 'euclid-cli eam login' again\n";
                return;
            }
            std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
        }

    }// namespace

    EkvCli::EkvCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath)
        : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EkvCli::process(const std::string &action, const std::vector<std::string> &args) const {

        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ekv", {
                                           {"create-table", "Create a table and say what its items are keyed on"},
                                           {"delete-item", "Remove one item by its key"},
                                           {"delete-table", "Delete a table and everything in it"},
                                           {"describe-table", "Show a table's key and how many items it holds"},
                                           {"get-item", "Read one item by its key"},
                                           {"list-tables", "List the account's tables"},
                                           {"put-item", "Write an item, replacing whatever was under its key"},
                                           {"query", "Read one partition's items, in sort-key order"},
                                           {"scan", "Read a table's items, a page at a time"},
                                   });
        }

        if (action == "create-table") return createTable(args);
        if (action == "describe-table") return describeTable(args);
        if (action == "list-tables") return listTables(args);
        if (action == "delete-table") return deleteTable(args);
        if (action == "put-item") return putItem(args);
        if (action == "get-item") return getItem(args);
        if (action == "delete-item") return deleteItem(args);
        if (action == "query") return query(args);
        if (action == "scan") return scan(args);

        std::cerr << "error: unknown EKV action '" << action << "'\n";
        return 1;
    }

    bool EkvCli::readJson(const po::variables_map &vm, const std::string &option, boost::json::value &value, std::string &error) {

        const auto fileOption = option + "-file";
        if (vm.contains(option) && vm.contains(fileOption)) {
            error = "give " + option + " once - --" + option + " or --" + fileOption;
            return false;
        }

        std::string text;
        if (vm.contains(option)) {
            text = vm[option].as<std::string>();
        } else if (vm.contains(fileOption)) {
            const auto path = vm[fileOption].as<std::string>();
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                error = "could not open '" + path + "'";
                return false;
            }
            std::ostringstream contents;
            contents << in.rdbuf();
            text = contents.str();
        } else {
            error = "--" + option + " or --" + fileOption + " is required";
            return false;
        }

        boost::system::error_code ec;
        value = boost::json::parse(text, ec);
        if (ec) {
            error = "--" + option + " is not valid JSON: " + ec.message();
            return false;
        }
        return true;
    }

    // ── Tables ───────────────────────────────────────────────────────────────

    int EkvCli::createTable(const std::vector<std::string> &args) const {

        po::options_description desc("ekv create-table options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "table name")
                ("partition-key,p", po::value<std::string>()->required(), "attribute every item is identified by")
                ("partition-key-type", po::value<std::string>()->default_value("string"), "its type: string, number or binary")
                ("sort-key,s", po::value<std::string>(), "attribute items within a partition are ordered by")
                ("sort-key-type", po::value<std::string>()->default_value("string"), "its type: string, number or binary");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "create-table", "--name <table> --partition-key <attribute> [--sort-key <attribute>]",
                                   "Creates a table. Every item written to it has to carry the partition key attribute, and "
                                   "the sort key attribute if one is declared - that pair is what identifies an item, and "
                                   "what a query looks items up by. A table with a sort key can be read as a range "
                                   "(see euclid-cli-ekv-query(1)); one without can only be read an item at a time.",
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

        Dto::EKV::CreateTableRequest request;
        request.name = vm["name"].as<std::string>();
        request.partitionKeyName = vm["partition-key"].as<std::string>();
        request.partitionKeyType = vm["partition-key-type"].as<std::string>();
        if (vm.contains("sort-key")) {
            request.sortKeyName = vm["sort-key"].as<std::string>();
            request.sortKeyType = vm["sort-key-type"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "create-table", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("create-table", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkvCli::describeTable(const std::vector<std::string> &args) const {

        po::options_description desc("ekv describe-table options");
        desc.add_options()("name,n", po::value<std::string>()->required(), "table name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "describe-table", "--name <table>",
                                   "Shows what a table is keyed on and how many items it holds. The count is counted "
                                   "when asked rather than kept, so it is always right and costs a query.",
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

        Dto::EKV::DescribeTableRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "describe-table", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("describe-table", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkvCli::listTables(const std::vector<std::string> &args) const {

        po::options_description desc("ekv list-tables options");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "only tables whose name starts with this")
                ("page-size", po::value<long>(), "most to return")
                ("page-index", po::value<long>(), "zero-based page")
                ("sort-column", po::value<std::string>(), "field to sort by; default is the name")
                ("sort-direction", po::value<std::string>(), "asc or desc");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "list-tables", "[--prefix <text>] [--page-size <n>] [--page-index <n>]",
                                   "Lists the tables of the account you are logged in to, with their keys and item counts.",
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

        Dto::EKV::ListTablesRequest request;
        if (vm.contains("prefix")) request.prefix = vm["prefix"].as<std::string>();
        if (vm.contains("page-size")) request.pageSize = vm["page-size"].as<long>();
        if (vm.contains("page-index")) request.pageIndex = vm["page-index"].as<long>();
        if (vm.contains("sort-column")) request.sortColumn = vm["sort-column"].as<std::string>();
        if (vm.contains("sort-direction")) request.sortDirection = vm["sort-direction"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "list-tables", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("list-tables", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkvCli::deleteTable(const std::vector<std::string> &args) const {

        po::options_description desc("ekv delete-table options");
        desc.add_options()("name,n", po::value<std::string>()->required(), "table name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "delete-table", "--name <table>",
                                   "Deletes a table and every item in it. There is no undo and no recycle bin: what the "
                                   "table held is gone when this returns.",
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

        Dto::EKV::DeleteTableRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "delete-table", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("delete-table", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    // ── Items ────────────────────────────────────────────────────────────────

    int EkvCli::putItem(const std::vector<std::string> &args) const {

        po::options_description desc("ekv put-item options");
        desc.add_options()
                ("table,t", po::value<std::string>()->required(), "table to write to")
                ("item,i", po::value<std::string>(), "the item, as a JSON object")
                ("item-file", po::value<std::string>(), "a file holding the item as JSON");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "put-item", "--table <table> --item <json> | --item-file <path>",
                                   "Writes an item, replacing whatever was stored under its key. The item is an ordinary "
                                   "JSON object and may nest as deeply as you like; it has to carry the table's key "
                                   "attributes, with the types the table declared. Numbers come back as numbers and "
                                   "strings as strings - what you write is what you read.",
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

        Dto::EKV::PutItemRequest request;
        request.table = vm["table"].as<std::string>();

        std::string error;
        if (!readJson(vm, "item", request.item, error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "put-item", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("put-item", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkvCli::getItem(const std::vector<std::string> &args) const {

        po::options_description desc("ekv get-item options");
        desc.add_options()
                ("table,t", po::value<std::string>()->required(), "table to read from")
                ("key,k", po::value<std::string>(), "the key attributes, as a JSON object")
                ("key-file", po::value<std::string>(), "a file holding the key as JSON");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "get-item", "--table <table> --key <json>",
                                   "Reads one item by its key. The key is an object of the table's key attributes and "
                                   "nothing else, e.g. '{\"supplierId\":\"4711\"}'. Answers 404 when there is no such "
                                   "item, which is not the same as an item with nothing in it.",
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

        Dto::EKV::GetItemRequest request;
        request.table = vm["table"].as<std::string>();

        std::string error;
        if (!readJson(vm, "key", request.key, error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "get-item", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("get-item", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkvCli::deleteItem(const std::vector<std::string> &args) const {

        po::options_description desc("ekv delete-item options");
        desc.add_options()
                ("table,t", po::value<std::string>()->required(), "table to remove from")
                ("key,k", po::value<std::string>(), "the key attributes, as a JSON object")
                ("key-file", po::value<std::string>(), "a file holding the key as JSON");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "delete-item", "--table <table> --key <json>",
                                   "Removes one item by its key. Says whether there was one to remove.",
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

        Dto::EKV::DeleteItemRequest request;
        request.table = vm["table"].as<std::string>();

        std::string error;
        if (!readJson(vm, "key", request.key, error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "delete-item", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("delete-item", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkvCli::query(const std::vector<std::string> &args) const {

        po::options_description desc("ekv query options");
        desc.add_options()
                ("table,t", po::value<std::string>()->required(), "table to query")
                ("partition-key,p", po::value<std::string>()->required(), "the partition key's value, as JSON (e.g. '\"4711\"' or '4711')")
                ("operator,o", po::value<std::string>(), "how to narrow by sort key: eq, lt, le, gt, ge, between, begins-with")
                ("value,v", po::value<std::string>(), "what to compare the sort key against, as JSON; the lower bound for between")
                ("upper,u", po::value<std::string>(), "the upper bound, for between")
                ("descending,d", po::bool_switch(), "read in descending sort-key order")
                ("page-size", po::value<long>(), "most to return")
                ("page-index", po::value<long>(), "zero-based page");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "query", "--table <table> --partition-key <json> [--operator <op> --value <json>]",
                                   "Reads the items of one partition, in sort-key order. Values are given as JSON, so a "
                                   "string key is quoted and a number key is not: --partition-key '\"4711\"' against a "
                                   "string key, --partition-key 4711 against a number one. Without an operator the whole "
                                   "partition comes back; with one, only the part of it the sort key selects.",
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

        Dto::EKV::QueryRequest request;
        request.table = vm["table"].as<std::string>();
        request.forward = !vm["descending"].as<bool>();
        if (vm.contains("page-size")) request.pageSize = vm["page-size"].as<long>();
        if (vm.contains("page-index")) request.pageIndex = vm["page-index"].as<long>();

        std::string error;
        if (!readJson(vm, "partition-key", request.partitionKey, error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        if (vm.contains("operator")) {
            request.sortOperator = vm["operator"].as<std::string>();
            if (!vm.contains("value")) {
                std::cerr << "error: --operator needs a --value to compare against\n";
                return 1;
            }
            if (!readJson(vm, "value", request.sortValue, error)) {
                std::cerr << "error: " << error << "\n";
                return 1;
            }
            if (vm.contains("upper") && !readJson(vm, "upper", request.sortUpper, error)) {
                std::cerr << "error: " << error << "\n";
                return 1;
            }
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "query", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("query", response);
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EkvCli::scan(const std::vector<std::string> &args) const {

        po::options_description desc("ekv scan options");
        desc.add_options()
                ("table,t", po::value<std::string>()->required(), "table to scan")
                ("page-size", po::value<long>(), "most to return")
                ("page-index", po::value<long>(), "zero-based page");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ekv", "scan", "--table <table> [--page-size <n>] [--page-index <n>]",
                                   "Reads a table's items in key order, a page at a time. It reads everything, which is "
                                   "what a scan is: use it to enumerate, export or debug a table, and a query to serve "
                                   "requests.",
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

        Dto::EKV::ScanRequest request;
        request.table = vm["table"].as<std::string>();
        if (vm.contains("page-size")) request.pageSize = vm["page-size"].as<long>();
        if (vm.contains("page-index")) request.pageIndex = vm["page-index"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ekv", "scan", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                reportFailure("scan", response);
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
