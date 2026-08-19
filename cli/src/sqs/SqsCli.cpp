#include <euclid/cli/sqs/SqsCli.h>

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

    SqsCli::SqsCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int SqsCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("sqs", {
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
        std::cerr << "error: unknown SQS action '" << action << "'\n";
        return 1;
    }

    int SqsCli::createQueue(const std::vector<std::string> &args) const {
        po::options_description desc("create queue options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name")
                ("visibility,v", po::value<long>()->default_value(30), "visibility in seconds")
                ("max-retries,m", po::value<long>()->default_value(3), "maximal number of retries")
                ("max-length,l", po::value<long>()->default_value(1024 * 1024), "maximal message length")
                ("dlq-name,d", po::value<std::string>(), "name of the dead letter queue");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "create-queue", "--name <name> [--visibility <seconds>] [--max-retries <value>] [--max-length <value>] [dlq-name <name>]",
                                   "Creates a new SQS queue with the given name, max retries, max message length, dead letter queue ARN, and visibility timeout. The dead letter queue name is optional; "
                                   "visibility defaults to 30 seconds, max retries defaults to 3, and max message length defaults to 1MB.",
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

        Dto::SQS::CreateQueueRequest request;
        request.name = vm["name"].as<std::string>();
        request.visibility = vm["visibility"].as<long>();
        request.maxRetries = vm["max-retries"].as<long>();
        request.maxMessageLength = vm["max-length"].as<long>();
        if (vm.contains("dlq-name")) {
            request.dlqName = vm["dlq-name"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "create-queue", boost::json::value_from(request));
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

    int SqsCli::listMessages(const std::vector<std::string> &args) const {
        po::options_description desc("lists a queue's messages without receiving them");
        desc.add_options()
                ("queue-ern,e", po::value<std::string>()->required(), "queue ERN")
                ("page-size,s", po::value<long>()->default_value(10), "page size")
                ("page-index,i", po::value<long>()->default_value(0), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("created"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "list-messages", "--queue-ern <ern> [--page-size <n>] [--page-index <n>] [--sort-column <column>]",
                                   "Lists a queue's messages without receiving them, i.e. without changing their status or visibility. Paginated: pageSize defaults to 10, pageIndex to 0, sortColumn to \"created\".",
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

        Dto::SQS::ListMessagesRequest request;
        request.queueErn = vm["queue-ern"].as<std::string>();
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "list-messages", boost::json::value_from(request));
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

    int SqsCli::getQueueErn(const std::vector<std::string> &args) const {
        po::options_description desc("get queue ern options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "get-queue-ern", "--name <name>",
                                   "Resolves the Euclid resource name (ERN) of a queue by its name.",
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

        Dto::SQS::CreateQueueRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "get-queue-ern", boost::json::value_from(request));
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

    int SqsCli::listQueues(const std::vector<std::string> &args) const {
        po::options_description desc("list queues options");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "queue name prefix")
                ("pageSize,s", po::value<long>()->default_value(-1), "page size")
                ("pageIndex,i", po::value<long>()->default_value(-1), "page index")
                ("sortColumn,c", po::value<std::string>()->default_value("name"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "list-queues", "[--prefix <prefix>] [--pageSize <n>] [--pageIndex <n>] [--sortColumn <column>]",
                                   "Lists SQS queues, optionally filtered by name prefix and paginated.",
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

        Dto::SQS::ListQueueRequest request;
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }
        request.pageSize = vm["pageSize"].as<long>();
        request.pageIndex = vm["pageIndex"].as<long>();
        request.sortColumn = vm["sortColumn"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "list-queues", boost::json::value_from(request));
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

    int SqsCli::purgeQueue(const std::vector<std::string> &args) const {
        po::options_description desc("purge queue options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "euclid resource name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "purge-queue", "--ern <ern>",
                                   "Deletes all messages from a SQS queue identified by its Euclid resource name (ERN).",
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

        Dto::SQS::PurgeQueueRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("sqs", "purge-queue", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: purge-queue failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int SqsCli::purgeAllQueues(const std::vector<std::string> &args) const {
        po::options_description desc("purge all queues options");
        desc.add_options()
                ("region,r", po::value<std::string>()->required(), "region")
                ("accountId,a", po::value<std::string>()->required(), "account ID");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "purge-all-queues", "--region <region> --accountId <accountId>",
                                   "Deletes all messages from every SQS queue in the given region and account.",
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

        Dto::SQS::PurgeAllQueuesRequest request;
        request.region = vm["region"].as<std::string>();
        request.accountId = vm["accountId"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("sqs", "purge-all-queues", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: purge-all-queues failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int SqsCli::getQueueMetadata(const std::vector<std::string> &args) const {
        po::options_description desc("returns the metadata of a queue");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "queue ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "get-queue-metadata", "--ern <ern>",
                                   "Shows the metadata of a queue, i.e. region, accountId, namespace, size, number of messages.",
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

        Dto::SQS::GetQueueMetadataRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "get-queue-metadata", boost::json::value_from(request));
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

    int SqsCli::deleteQueue(const std::vector<std::string> &args) const {
        po::options_description desc("delete queue options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "euclid resource name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "delete-queue", "--ern <ern>",
                                   "Deletes an SQS queue identified by its Euclid resource name (ERN).",
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

        Dto::SQS::DeleteQueueRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("sqs", "delete-queue", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-queue failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int SqsCli::sendMessage(const std::vector<std::string> &args) const {
        po::options_description desc("send message options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "queue resource name")
                ("body,b", po::value<std::string>()->required(), "message body")
                ("attributes,a", po::value<std::string>(), "message attributes");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "send-message", "--ern <ern> --body <body|file://path> [--attributes <json|file://path>]",
                                   "Sends a message to an SQS queue. If --body starts with 'file://', the message "
                                   "body is read from the referenced file instead of being taken literally. The optional --attributes value sets the message "
                                   "attributes as a JSON object mapping attribute name to {\"type\": <int|long|double|float|bool|string|binary>, \"value\": <value>}, "
                                   "given either literally or via 'file://path' to a file containing the JSON.",
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

        Dto::SQS::SendMessageRequest request;
        request.queueErn = vm["ern"].as<std::string>();

        try {
            request.body = ResolveFileOrLiteral(vm["body"].as<std::string>());
            if (vm.contains("attributes")) {
                const std::string attributesJson = ResolveFileOrLiteral(vm["attributes"].as<std::string>());
                const boost::json::value attributesValue = Core::ParseJsonString(attributesJson);
                for (const auto &attribute: attributesValue.as_object()) {
                    request.attributes.emplace(attribute.key(), boost::json::value_to<Dto::SQS::Variant>(attribute.value()));
                }
            }
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "send-message", boost::json::value_from(request));
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

    int SqsCli::receiveMessages(const std::vector<std::string> &args) const {
        po::options_description desc("receive messages options");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "queue resource name")
                ("maxCount,m", po::value<long>()->default_value(10), "maximal message count")
                ("waitTime,w", po::value<long>()->default_value(0), "maximal waiting time in seconds");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "receive-messages", "--ern <ern> [--maxCount <value>] [--waitTime <seconds>]",
                                   "Receive messages from an SQS queue. If messages are available return up to maxCount messages.",
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

        Dto::SQS::ReceiveMessagesRequest request;
        request.ern = vm["ern"].as<std::string>();
        request.maxCount = vm["maxCount"].as<long>();
        request.waitTime = vm["waitTime"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "receive-messages", boost::json::value_from(request));
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

    int SqsCli::setVisibility(const std::vector<std::string> &args) const {
        po::options_description desc("sets the message visibility");
        desc.add_options()
                ("message-id,m", po::value<std::string>()->required(), "message ID")
                ("visibility,v", po::value<long>()->default_value(10), "visibility in seconds");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "set-visibility", "--message-id <messageId> --visibility <seconds>",
                                   "Sets the visibility timeout for an individual message.",
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

        Dto::SQS::SetMessageVisibilityRequest request;
        request.messageId = vm["message-id"].as<std::string>();
        request.visibility = vm["visibility"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("sqs", "set-visibility", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: set-visibility failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int SqsCli::getMessageCount(const std::vector<std::string> &args) const {
        po::options_description desc("returns the message counter");
        desc.add_options()
                ("ern,e", po::value<std::string>()->required(), "queue ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "get-message-count", "--ern <ERN>",
                                   "Returns the message counter, like total, invisible, delayed.",
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

        Dto::SQS::GetMessageCountRequest request;
        request.ern = vm["ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "get-message-count", boost::json::value_from(request));
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

    int SqsCli::getMessageAttribute(const std::vector<std::string> &args) const {
        po::options_description desc("returns a message attribute by name");
        desc.add_options()
                ("message-id,m", po::value<std::string>()->required(), "message ID")
                ("name,n", po::value<std::string>()->required(), "attribute name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "get-message-attribute", "--message-id <messageId> --name <name>",
                                   "Returns a single message attribute by name.",
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

        Dto::SQS::GetMessageAttributeRequest request;
        request.messageId = vm["message-id"].as<std::string>();
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "get-message-attribute", boost::json::value_from(request));
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

    int SqsCli::getMessageMetadata(const std::vector<std::string> &args) const {
        po::options_description desc("returns the metadata of a message");
        desc.add_options()
                ("message-id,m", po::value<std::string>()->required(), "message ID");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("sqs", "get-message-metadata", "--message-id <messageId>",
                                   "Shows a message's metadata: messageId, queueErn, receiptHandle, status, size (bytes), receivedCount, visibilityTimeout, contentType, md5Body, md5Attributes, created, modified.",
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

        Dto::SQS::GetMessageMetadataRequest request;
        request.messageId = vm["message-id"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("sqs", "get-message-metadata", boost::json::value_from(request));
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

}