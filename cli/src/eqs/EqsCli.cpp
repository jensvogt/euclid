#include <euclid/cli/eqs/EqsCli.h>

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
            const std::ifstream in(path);
            if (!in.is_open()) {
                throw std::runtime_error("could not open file '" + path + "'");
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

    }// namespace

    EqsCli::EqsCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EqsCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("eqs", {
                                                  {"create-queue", "Create a new queue"},
                                                  {"list-queues", "List queues"},
                                                  {"list-messages", "List a queue's messages without receiving them"},
                                                  {"get-queue-ern", "Resolve a queue's ERN by name"},
                                                  {"get-message-count", "Returns the message counters"},
                                                  {"purge-queue", "Delete all messages from a queue"},
                                                  {"purge-all-queues", "Delete all messages from every queue in a region/account"},
                                                  {"delete-queue", "Delete a queue"},
                                                  {"send-message", "Send a message to a queue"},
                                                  {"receive-messages", "Send a message to a queue"},
                                                  {"set-visibility", "Send a messages visibility"},
                                                  {"get-message-attribute", "Return a message attribute by name"},
                                                  {"get-queue-metadata", "Return the metadata for a queue"},
                                                  {"get-message-metadata", "Return the metadata for a message"},
                                                  {"add-queue-tag", "Adds a tag to queue"},
                                                  {"delete-queue-tag", "Deletes a tag from the queue"},
                                          });
        }
        if (action == "create-queue") {
            return createQueue(args);
        }
        if (action == "list-queues") {
            return listQueues(args);
        }
        if (action == "list-messages") {
            return listMessages(args);
        }
        if (action == "get-queue-ern") {
            return getQueueErn(args);
        }
        if (action == "purge-queue") {
            return purgeQueue(args);
        }
        if (action == "purge-all-queues") {
            return purgeAllQueues(args);
        }
        if (action == "delete-queue") {
            return deleteQueue(args);
        }
        if (action == "send-message") {
            return sendMessage(args);
        }
        if (action == "receive-messages") {
            return receiveMessages(args);
        }
        if (action == "set-visibility") {
            return setVisibility(args);
        }
        if (action == "get-message-count") {
            return getMessageCount(args);
        }
        if (action == "get-message-attribute") {
            return getMessageAttribute(args);
        }
        if (action == "get-queue-metadata") {
            return getQueueMetadata(args);
        }
        if (action == "get-message-metadata") {
            return getMessageMetadata(args);
        }
        if (action == "add-queue-tag") {
            return addQueueTag(args);
        }
        if (action == "delete-queue-tag") {
            return deleteQueueTag(args);
        }
        std::cerr << "error: unknown SQS action '" << action << "'\n";
        return 1;
    }

    int EqsCli::createQueue(const std::vector<std::string> &args) const {
        po::options_description desc("create queue options");
        desc.add_options()("name,n", po::value<std::string>()->required(), "name")("visibility,v", po::value<long>()->default_value(30), "visibility in seconds")("max-retries,m", po::value<long>()->default_value(3), "maximal number of retries")("max-length,l", po::value<long>()->default_value(1024 * 1024), "maximal message length")("dlq-name,d", po::value<std::string>(), "name of the dead letter queue")("delay,e", po::value<long>()->default_value(0), "message delay");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "create-queue", "--name <name> [--visibility <seconds>] [--max-retries <value>] [--max-length <value>] [dlq-name <name>] [--delay <seconds>]",
                                   "Creates a new SQS queue with the given name, max retries, max message length, dead letter queue ARN, and visibility timeout. The dead letter queue name is optional; "
                                   "visibility defaults to 30 seconds, max retries defaults to 3, delay to 0, and max message length defaults to 1MB.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::CreateQueueRequest request;
        request.name = vm["name"].as<std::string>();
        request.visibility = vm["visibility"].as<long>();
        request.maxRetries = vm["max-retries"].as<long>();
        request.maxMessageLength = vm["max-length"].as<long>();
        if (vm.contains("dlq-name")) {
            request.dlqName = vm["dlq-name"].as<std::string>();
        }
        if (vm.contains("delay")) {
            request.delay = vm["delay"].as<long>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "create-queue", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: create-queue failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::listMessages(const std::vector<std::string> &args) const {
        po::options_description desc("lists a queue's messages without receiving them");
        desc.add_options()("queue-ern,e", po::value<std::string>()->required(), "queue ERN")("page-size,s", po::value<long>()->default_value(10), "page size")("page-index,i", po::value<long>()->default_value(0), "page index")("sort-column,c", po::value<std::string>()->default_value("created"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "list-messages", "--queue-ern <ern> [--page-size <n>] [--page-index <n>] [--sort-column <column>]",
                                   "Lists a queue's messages without receiving them, i.e. without changing their status or visibility. Paginated: pageSize defaults to 10, pageIndex to 0, sortColumn to \"created\".",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::ListMessagesRequest request;
        request.queueErn = vm["queue-ern"].as<std::string>();
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "list-messages", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-messages failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::getQueueErn(const std::vector<std::string> &args) const {
        po::options_description desc("get queue ern options");
        desc.add_options()("name,n", po::value<std::string>()->required(), "name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "get-queue-ern", "--name <name>",
                                   "Resolves the Euclid resource name (ERN) of a queue by its name.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::CreateQueueRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "get-queue-ern", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-queue-ern failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::listQueues(const std::vector<std::string> &args) const {
        po::options_description desc("list queues options");
        desc.add_options()("prefix,p", po::value<std::string>(), "queue name prefix")("pageSize,s", po::value<long>()->default_value(-1), "page size")("pageIndex,i", po::value<long>()->default_value(-1), "page index")("sortColumn,c", po::value<std::string>()->default_value("name"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "list-queues", "[--prefix <prefix>] [--pageSize <n>] [--pageIndex <n>] [--sortColumn <column>]",
                                   "Lists SQS queues, optionally filtered by name prefix and paginated.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::ListQueueRequest request;
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }
        request.pageSize = vm["pageSize"].as<long>();
        request.pageIndex = vm["pageIndex"].as<long>();
        request.sortColumn = vm["sortColumn"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "list-queues", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-queues failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::purgeQueue(const std::vector<std::string> &args) const {
        po::options_description desc("purge queue options");
        desc.add_options()("ern,e", po::value<std::string>()->required(), "euclid resource name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "purge-queue", "--ern <ern>",
                                   "Deletes all messages from a SQS queue identified by its Euclid resource name (ERN).",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::PurgeQueueRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("eqs", "purge-queue", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: purge-queue failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::purgeAllQueues(const std::vector<std::string> &args) const {
        po::options_description desc("purge all queues options");
        desc.add_options()("region,r", po::value<std::string>()->required(), "region")("accountId,a", po::value<std::string>()->required(), "account ID");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "purge-all-queues", "--region <region> --accountId <accountId>",
                                   "Deletes all messages from every SQS queue in the given region and account.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::PurgeAllQueuesRequest request;
        request.region = vm["region"].as<std::string>();
        request.accountId = vm["accountId"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("eqs", "purge-all-queues", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: purge-all-queues failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::getQueueMetadata(const std::vector<std::string> &args) const {
        po::options_description desc("returns the metadata of a queue");
        desc.add_options()("ern,e", po::value<std::string>()->required(), "queue ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "get-queue-metadata", "--ern <ern>",
                                   "Shows the metadata of a queue, i.e. region, accountId, namespace, size, number of messages.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::GetQueueMetadataRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "get-queue-metadata", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-queue-metadata failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::deleteQueue(const std::vector<std::string> &args) const {
        po::options_description desc("delete queue options");
        desc.add_options()("ern,e", po::value<std::string>()->required(), "euclid resource name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "delete-queue", "--ern <ern>",
                                   "Deletes an SQS queue identified by its Euclid resource name (ERN).",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::DeleteQueueRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("eqs", "delete-queue", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-queue failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::sendMessage(const std::vector<std::string> &args) const {
        po::options_description desc("send message options");
        desc.add_options()("ern,e", po::value<std::string>()->required(), "queue resource name")("body,b", po::value<std::string>()->required(), "message body")("attributes,a", po::value<std::string>(), "message attributes")("priority,p", po::value<std::string>()->default_value("MIDDLE"), "message priority (LOW|MIDDLE|HIGH)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "send-message", "--ern <ern> --body <body|file://path> [--attributes <json|file://path>] [--priority <LOW|MIDDLE|HIGH>]",
                                   "Sends a message to an SQS queue. If --body starts with 'file://', the message "
                                   "body is read from the referenced file instead of being taken literally. The optional --attributes value sets the message "
                                   "attributes as a JSON object mapping attribute name to {\"type\": <int|long|double|float|bool|string|binary>, \"value\": <value>}, "
                                   "given either literally or via 'file://path' to a file containing the JSON. --priority defaults to MIDDLE and influences how "
                                   "the message is prioritized by receive-messages.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::SendMessageRequest request;
        request.queueErn = vm["ern"].as<std::string>();
        request.priority = vm["priority"].as<std::string>();

        try {
            request.body = ResolveFileOrLiteral(vm["body"].as<std::string>());
            if (vm.contains("attributes")) {
                const std::string attributesJson = ResolveFileOrLiteral(vm["attributes"].as<std::string>());
                const boost::json::value attributesValue = Core::ParseJsonString(attributesJson);
                for (const auto &attribute: attributesValue.as_object()) {
                    request.attributes.emplace(attribute.key(), boost::json::value_to<Dto::EQS::Variant>(attribute.value()));
                }
            }
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "send-message", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: send-message failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::receiveMessages(const std::vector<std::string> &args) const {
        po::options_description desc("receive messages options");
        desc.add_options()("ern,e", po::value<std::string>()->required(), "queue resource name")("maxCount,m", po::value<long>()->default_value(10), "maximal message count")("waitTime,w", po::value<long>()->default_value(0), "maximal waiting time in seconds");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "receive-messages", "--ern <ern> [--maxCount <value>] [--waitTime <seconds>]",
                                   "Receive messages from an SQS queue. If messages are available return up to maxCount messages. "
                                   "The returned messages favor higher priority ones: maxCount slots are split across LOW/MIDDLE/HIGH priority "
                                   "proportionally to the server's configurable priority weights (4:2:1 by default), so most of a batch is "
                                   "HIGH priority, fewer MIDDLE, and fewer still LOW.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::ReceiveMessagesRequest request;
        request.ern = vm["ern"].as<std::string>();
        request.maxCount = vm["maxCount"].as<long>();
        request.waitTime = vm["waitTime"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "receive-messages", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: receive-messages failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::setVisibility(const std::vector<std::string> &args) const {
        po::options_description desc("sets the message visibility");
        desc.add_options()("message-id,m", po::value<std::string>()->required(), "message ID")("visibility,v", po::value<long>()->default_value(10), "visibility in seconds");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "set-visibility", "--message-id <messageId> --visibility <seconds>",
                                   "Sets the visibility timeout for an individual message.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::SetMessageVisibilityRequest request;
        request.messageId = vm["message-id"].as<std::string>();
        request.visibility = vm["visibility"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("eqs", "set-visibility", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: set-visibility failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::getMessageCount(const std::vector<std::string> &args) const {
        po::options_description desc("returns the message counter");
        desc.add_options()("ern,e", po::value<std::string>()->required(), "queue ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "get-message-count", "--ern <ERN>",
                                   "Returns the message counter, like total, invisible, delayed.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::GetMessageCountRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "get-message-count", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-message-count failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::getMessageAttribute(const std::vector<std::string> &args) const {
        po::options_description desc("returns a message attribute by name");
        desc.add_options()("message-id,m", po::value<std::string>()->required(), "message ID")("name,n", po::value<std::string>()->required(), "attribute name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "get-message-attribute", "--message-id <messageId> --name <name>",
                                   "Returns a single message attribute by name.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::GetMessageAttributeRequest request;
        request.messageId = vm["message-id"].as<std::string>();
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "get-message-attribute", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-message-attribute failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::getMessageMetadata(const std::vector<std::string> &args) const {
        po::options_description desc("get message metadata options");
        desc.add_options()("message-id,m", po::value<std::string>()->required(), "message ID");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "get-message-metadata", "--message-id <messageId>",
                                   "Shows a message's metadata: messageId, queueErn, receiptHandle, status, priority, size (bytes), receivedCount, visibilityTimeout, contentType, md5Body, md5Attributes, created, modified.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::GetMessageMetadataRequest request;
        request.messageId = vm["message-id"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eqs", "get-message-metadata", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-message-metadata failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::addQueueTag(const std::vector<std::string> &args) const {
        po::options_description desc("add queue tag options");
        desc.add_options()
        ("queue,q", po::value<std::string>()->required(), "queue ERN")
        ("key,k", po::value<std::string>()->required(), "tag key")
        ("value,v", po::value<std::string>()->required(), "tag value");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "add-queue-tag", "--queue <queue ERN> --key <value> --value <value>",
                                   "Adds a tag to a queue.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::EQS::AddQueueTagRequest request;
        request.ern = vm["queue"].as<std::string>();
        request.key = vm["key"].as<std::string>();
        request.value = vm["value"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("eqs", "add-queue-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: add-queue-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EqsCli::deleteQueueTag(const std::vector<std::string> &args) const {
        po::options_description desc("delete queue tag options");
        desc.add_options()
        ("queue,q", po::value<std::string>()->required(), "queue ERN")
        ("key,k", po::value<std::string>()->required(), "tag key");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "delete-queue-tag", "--queue <queue ERN> --key <value>",
                                   "Deletes a tag from a queue.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        Dto::ESM::DeleteBucketTagRequest request;
        request.ern = vm["queue"].as<std::string>();
        request.key = vm["key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("eqs", "delete-queue-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-queue-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }
}// namespace Euclid::CLI