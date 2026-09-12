// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <euclid/cli/ens/EnsCli.h>

// C++ includes
#include <limits>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Seconds, or a number with a unit: 90m, 36h, 14d. Nothing cleverer - a period is one
        // number and one unit, and anybody who wants a fortnight can write 14d rather than count
        // it out in seconds. Plus the one value that is not a length at all, -1.
        bool parseRetentionPeriod(const std::string &text, long &seconds) {

            if (text.empty()) return false;

            // "forever", spelled the way the API takes it. Accepted only on its own: "-1d" is not a
            // duration and not forever either, and reading it as one would be a guess.
            if (text == "-1" || text == "forever" || text == "never") {
                seconds = -1;
                return true;
            }

            long multiplier = 1;
            std::string digits = text;
            switch (const char last = text.back()) {
                case 's':
                case 'm':
                case 'h':
                case 'd':
                    multiplier = last == 's' ? 1 : last == 'm' ? 60 : last == 'h' ? 3600 : 86400;
                    digits = text.substr(0, text.size() - 1);
                    break;
                default:
                    break;
            }
            if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return false;

            try {
                seconds = std::stol(digits) * multiplier;
            } catch (const std::exception &) {
                return false;
            }
            return seconds >= 0;
        }

        // Bytes, or a number with a unit: 512k, 2M, 1G. Binary multiples, because the limit this
        // parses sits beside a default of 1024*1024 - an operator who writes 1M means that number,
        // not 1000000. Zero and negatives are refused here rather than sent: a topic that accepts
        // nothing is a mistake, and saying so costs no round trip.
        bool parseByteSize(const std::string &text, long &bytes) {

            if (text.empty()) return false;

            long multiplier = 1;
            std::string digits = text;
            switch (const char last = text.back()) {
                case 'b':
                case 'B':
                case 'k':
                case 'K':
                case 'm':
                case 'M':
                case 'g':
                case 'G':
                    multiplier = last == 'b' || last == 'B'   ? 1
                                 : last == 'k' || last == 'K' ? 1024
                                 : last == 'm' || last == 'M' ? 1024 * 1024
                                                              : 1024 * 1024 * 1024;
                    digits = text.substr(0, text.size() - 1);
                    break;
                default:
                    break;
            }
            if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return false;

            try {
                const long value = std::stol(digits);
                // Checked before multiplying rather than after: "99999999999G" overflows silently
                // and would arrive at the server as whatever the wrap happened to produce.
                if (value > std::numeric_limits<long>::max() / multiplier) return false;
                bytes = value * multiplier;
            } catch (const std::exception &) {
                return false;
            }
            return bytes > 0;
        }

    }// namespace

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

    EnsCli::EnsCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EnsCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ens", {
                                           {"add-topic-tag", "Adds a tag to a topic"},
                                           {"create-topic", "Create a new topic"},
                                           {"delete-topic", "Delete an existing topic"},
                                           {"delete-topic-tag", "Deletes a tag from a topic"},
                                           {"get-message-attribute", "Returns a message attribute"},
                                           {"get-message-count", "Returns the number of messages in a topic"},
                                           {"get-topic-ern", "Returns the ERN for a topic"},
                                           {"get-topic-metadata", "Returns the metadata of a topics"},
                                           {"list-messages", "List available messages"},
                                           {"list-subscriptions", "Lists the subscriptions of a topic"},
                                           {"list-topics", "List all available topics"},
                                           {"publish-message", "Publish a message to a topic"},
                                           {"purge-all-topic", "Purge all topics by deleting all messages"},
                                           {"purge-topic", "Purge a topic by deleting all messages"},
                                           {"set-message-attribute", "Sets the value of a message attribute"},
                                           {"set-topic-max-message-length", "Sets the largest message a topic accepts"},
                                           {"set-topic-retention", "Sets how long a topic keeps the messages published to it"},
                                           {"start-topic", "Starts delivering to a topic's subscribers, handing over what it held"},
                                           {"stop-topic", "Stops delivering to a topic's subscribers; publishes are still stored"},
                                           {"set-topic-tag", "Sets the value of an existing topic tag"},
                                           {"subscribe", "Subscribes a target resource (an EQS queue) to a topic"},
                                           {"unsubscribe", "Deletes a subscription"},
                                   });
        }
        if (action == "create-topic") {
            return createTopic(args);
        }
        if (action == "list-topics") {
            return listTopics(args);
        }
        if (action == "get-topic-ern") {
            return getTopicErn(args);
        }
        if (action == "get-topic-metadata") {
            return getTopicMetadata(args);
        }
        if (action == "publish-message") {
            return publishMessage(args);
        }
        if (action == "purge-topic") {
            return purgeTopic(args);
        }
        if (action == "purge-all-topic") {
            return purgeAllTopic(args);
        }
        if (action == "add-topic-tag") {
            return addTopicTag(args);
        }
        if (action == "set-topic-tag") {
            return setTopicTag(args);
        }
        if (action == "delete-topic-tag") {
            return deleteTopicTag(args);
        }
        if (action == "set-topic-retention") {
            return setTopicRetention(args);
        }
        if (action == "set-topic-max-message-length") {
            return setTopicMaxMessageLength(args);
        }
        if (action == "start-topic") {
            return setTopicDelivering(args, true);
        }
        if (action == "stop-topic") {
            return setTopicDelivering(args, false);
        }
        if (action == "delete-topic") {
            return deleteTopic(args);
        }
        if (action == "list-messages") {
            return listMessages(args);
        }
        if (action == "get-message-count") {
            return getMessageCount(args);
        }
        if (action == "get-message-attribute") {
            return getMessageAttribute(args);
        }
        if (action == "set-message-attribute") {
            return setMessageAttribute(args);
        }
        // if (action == "get-message-metadata") {
        //     return getMessageMetadata(args);
        // }
        if (action == "add-topic-tag") {
            return addTopicTag(args);
        }
        if (action == "set-topic-tag") {
            return setTopicTag(args);
        }
        if (action == "delete-topic-tag") {
            return deleteTopicTag(args);
        }
        if (action == "subscribe") {
            return subscribe(args);
        }
        if (action == "unsubscribe") {
            return unsubscribe(args);
        }
        if (action == "list-subscriptions") {
            return listSubscriptions(args);
        }
        std::cerr << "error: unknown ENS action '" << action << "'\n";
        return 1;
    }

    int EnsCli::createTopic(const std::vector<std::string> &args) const {
        po::options_description desc("create topic options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "name")
                ("max-length,l", po::value<long>()->default_value(1024 * 1024), "maximal message length");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "create-topic", "--name <name> [--max-length <value>]",
                                   "Creates a new ENS topic with the given name, max message length. Max message length defaults to 1MB.",
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

        Dto::ENS::CreateTopicRequest request;
        request.name = vm["name"].as<std::string>();
        request.maxMessageLength = vm["max-length"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "create-topic", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: create-topic failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::listMessages(const std::vector<std::string> &args) const {
        po::options_description desc("lists a topic's messages without receiving them");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace")
                ("page-size,s", po::value<long>()->default_value(10), "page size")
                ("page-index,i", po::value<long>()->default_value(0), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("created"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "list-messages", "--topic <name|ern> [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists a topic's messages without receiving them, i.e. without changing their status. Paginated: page-size defaults to 10, page-index to 0, sort-column to \"created\""
                                   "and sort-direction to \"asc\".",
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

        Dto::ENS::ListMessagesRequest request;
        request.topicErn = vm["topic"].as<std::string>();
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        request.sortDirection = vm["sort-direction"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "list-messages", boost::json::value_from(request));
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

    int EnsCli::getTopicErn(const std::vector<std::string> &args) const {
        po::options_description desc("get topic ern options");
        desc.add_options()("name,n", po::value<std::string>()->required(), "name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "get-topic-ern", "--name <name>",
                                   "Resolves the Euclid resource name (ERN) of a topic by its name.",
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

        Dto::ENS::GetTopicErnRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "get-topic-ern", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-topic-ern failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::listTopics(const std::vector<std::string> &args) const {
        po::options_description desc("list topics options");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "queue name prefix")
                ("page-size,s", po::value<long>()->default_value(-1), "page size")
                ("page-index,i", po::value<long>()->default_value(-1), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "list-topics", "[--prefix <prefix>] [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists ENS queues, optionally filtered by name prefix and paginated.. Paginated: pageSize defaults to 10, pageIndex to 0,"
                                   " sortColumn to \"name\" and sortDirection to \"asc\".",
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

        Dto::ENS::ListTopicsRequest request;
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        request.sortDirection = vm["sort-direction"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "list-topics", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-topics failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::purgeTopic(const std::vector<std::string> &args) const {
        po::options_description desc("purge topic options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "purge-topic", "--topic <name|ern>",
                                   "Deletes all messages from a ENS topic identified by its Euclid resource name (ERN).",
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

        Dto::ENS::PurgeTopicRequest request;
        request.ern = vm["topic"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("ens", "purge-topic", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: purge-topic failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::purgeAllTopic(const std::vector<std::string> &args) const {
        po::options_description desc("purge all topics options");
        desc.add_options()
                ("region,r", po::value<std::string>()->required(), "region")
                ("accountId,a", po::value<std::string>()->required(), "account ID")
                ("namespace,n", po::value<std::string>(), "name space");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "purge-all-topics", "--region <region> --accountId <accountId> --namespace <namespace>",
                                   "Deletes all messages from every ENS topic in the given region, account and namespace. If no namespace is specified "
                                   "purges all topics of the region/accountId.",
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

        Dto::ENS::PurgeAllTopicsRequest request;
        request.region = vm["region"].as<std::string>();
        request.accountId = vm["accountId"].as<std::string>();
        // vm is keyed by the option's own name, "namespace" - asking it for "nameSpace" inserted an
        // empty value and threw bad_any_cast out of a spot no try block covers, so passing the
        // option this branch exists for was the one way to make the command abort.
        if (vm.contains("namespace")) {
            request.nameSpace = vm["namespace"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("ens", "purge-all-topics", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: purge-all-topics failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::getTopicMetadata(const std::vector<std::string> &args) const {
        po::options_description desc("returns the metadata of a topic");
        desc.add_options()("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "get-topic-metadata", "--ern <name|ern>",
                                   "Shows the metadata of a topic, i.e. region, accountId, namespace, size, number of messages.",
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

        Dto::ENS::GetTopicMetadataRequest request;
        request.ern = vm["topic"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "get-topic-metadata", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: get-topic-metadata failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::deleteTopic(const std::vector<std::string> &args) const {
        po::options_description desc("delete topic options");
        desc.add_options()("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "delete-topic", "--topic <name|ern>",
                                   "Deletes an ENS topic identified by its ERN.",
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

        Dto::ENS::DeleteTopicRequest request;
        request.ern = vm["topic"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("ens", "delete-topic", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-topic failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::publishMessage(const std::vector<std::string> &args) const {
        po::options_description desc("publish message options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace")
                ("body,b", po::value<std::string>()->required(), "message body")
                ("attributes,a", po::value<std::string>(), "message attributes");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "publish-message", "--topic <name|ern> --body <body|file://path> [--attributes <json|file://path>]",
                                   "Sends a message to an ENS topic. If --body starts with 'file://', the message "
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
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        Dto::ENS::PublishMessageRequest request;
        request.ern = vm["topic"].as<std::string>();

        try {
            request.body = ResolveFileOrLiteral(vm["body"].as<std::string>());
            if (vm.contains("attributes")) {
                const std::string attributesJson = ResolveFileOrLiteral(vm["attributes"].as<std::string>());
                for (const boost::json::value attributesValue = Core::ParseJsonString(attributesJson); const auto &attribute: attributesValue.as_object()) {
                    request.attributes.emplace(attribute.key(), boost::json::value_to<Dto::COM::Variant>(attribute.value()));
                }
            }
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "publish-message", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: publish-message failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::getMessageCount(const std::vector<std::string> &args) const {
        po::options_description desc("returns the message count");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eqs", "get-message-count", "--ern <ern>",
                                   "Returns the message counter, like available, send, resend.",
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

        Dto::ENS::GetMessageCountRequest request;
        request.ern = vm["topic"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "get-message-count", boost::json::value_from(request));
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

    int EnsCli::getMessageAttribute(const std::vector<std::string> &args) const {
        po::options_description desc("returns a message attribute by name");
        desc.add_options()
                ("message-id,m", po::value<std::string>()->required(), "message ID")
                ("key,k", po::value<std::string>()->required(), "attribute key");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "get-message-attribute", "--message-id <messageId> --key <key>",
                                   "Returns a single message attribute by key.",
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

        Dto::ENS::GetMessageAttributeRequest request;
        request.messageId = vm["message-id"].as<std::string>();
        request.key = vm["key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "get-message-attribute", boost::json::value_from(request));
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

    int EnsCli::setMessageAttribute(const std::vector<std::string> &args) const {
        po::options_description desc("sets the value of a message attribute");
        desc.add_options()
                ("message-id,m", po::value<std::string>()->required(), "message ID")
                ("key,k", po::value<std::string>()->required(), "attribute key")
                ("value,v", po::value<std::string>()->required(), "attribute value")
                ("type,t", po::value<std::string>(), "attribute type");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "set-message-attribute", "--message-id <messageId> --key <key> --value <value>",
                                   "Sets the value of a single message attribute. If the attributes does not exist yet, it will be created; "
                                   "if it exists already the value will be set. The type can be one of int, long, double, float, bool, string, binary. "
                                   "The type defaults to string.",
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

        Dto::ENS::SetMessageAttributeRequest request;
        request.messageId = vm["message-id"].as<std::string>();
        request.key = vm["key"].as<std::string>();
        request.value = optionToVariant(vm["value"].as<std::string>(), vm["type"].empty() ? "string" : vm["type"].as<std::string>());

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "set-message-attribute", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: set-message-attribute failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    //
    // int EqsCli::getMessageMetadata(const std::vector<std::string> &args) const {
    //     po::options_description desc("get message metadata options");
    //     desc.add_options()
    //             ("message-id,m", po::value<std::string>()->required(), "message ID");
    //
    //     if (IsHelpRequest(args)) {
    //         return PrintActionHelp("eqs", "get-message-metadata", "--message-id <messageId>",
    //                                "Shows a message's metadata: messageId, queueErn, receiptHandle, status, priority, size (bytes), receivedCount, visibilityTimeout, contentType, md5Body, md5Attributes, created, modified.",
    //                                desc);
    //     }
    //
    //     po::variables_map vm;
    //     try {
    //         po::store(po::command_line_parser(args).options(desc).run(), vm);
    //         po::notify(vm);
    //     } catch (const po::error &ex) {
    //         std::cerr << "error: " << ex.what() << "\n\n"
    //                 << desc << std::endl;
    //         return 1;
    //     }
    //
    //     Dto::EQS::GetMessageMetadataRequest request;
    //     request.messageId = vm["message-id"].as<std::string>();
    //
    //     try {
    //         const HttpClient client(_endpoint, _authentication, _caCertPath);
    //         const HttpResponse response = client.Post("eqs", "get-message-metadata", boost::json::value_from(request));
    //         if (!response.IsSuccess()) {
    //             std::cerr << "error: get-message-metadata failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
    //             return 1;
    //         }
    //         Core::WriteJson(std::cout, response.body, _pretty);
    //         return 0;
    //     } catch (const std::exception &ex) {
    //         std::cerr << "error: " << ex.what() << std::endl;
    //         return 1;
    //     }
    // }

    int EnsCli::addTopicTag(const std::vector<std::string> &args) const {
        po::options_description desc("add topic tag options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace")
                ("key,k", po::value<std::string>()->required(), "tag key")
                ("value,v", po::value<std::string>()->required(), "tag value");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "add-topic-tag", "--topic <name|ern> --key <value> --value <value>",
                                   "Adds a tag to a topic.",
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

        Dto::ENS::AddTopicTagRequest request;
        request.ern = vm["topic"].as<std::string>();
        request.key = vm["key"].as<std::string>();
        request.value = vm["value"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("ens", "add-topic-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: add-topic-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::setTopicTag(const std::vector<std::string> &args) const {
        po::options_description desc("set topic tag options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace")
                ("key,k", po::value<std::string>()->required(), "tag key")
                ("value,v", po::value<std::string>()->required(), "tag value");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "set-topic-tag", "--topic <name|ern> --key <value> --value <value>",
                                   "Sets the value of a topic tag. The topic tag must exist already, otherwise an error is returned.",
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

        Dto::ENS::SetTopicTagRequest request;
        request.ern = vm["topic"].as<std::string>();
        request.key = vm["key"].as<std::string>();
        request.value = vm["value"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("ens", "set-topic-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: set-topic-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::setTopicRetention(const std::vector<std::string> &args) const {
        po::options_description desc("set topic retention options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace")
                ("retention-period,r", po::value<std::string>()->required(),
                 "how long to keep a published message: seconds, or a duration such as 14d, 36h or 90m; "
                 "0 follows the installation default; -1 (or 'forever') never removes it");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "set-topic-retention", "--topic <name|ern> --retention-period <duration>",
                                   "Sets how long messages published to a topic are kept. Published messages are not "
                                   "consumed the way queue messages are - a topic fans them out and keeps what was "
                                   "published - so without a retention period they would stay forever, and every "
                                   "topic shares one collection. "
                                   "The period applies to messages published afterwards: the ones already stored keep "
                                   "the expiry they were given when they were published. A period of 0 means the topic "
                                   "has none of its own and follows euclid.modules.ens.retention-period, which is where "
                                   "every topic starts out and which defaults to 14 days. "
                                   "A period of -1, also spelled 'forever', stores each message with no expiry at all, "
                                   "so nothing ever removes it; that topic then grows without limit, and only "
                                   "purge-topic empties it.",
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

        long seconds = 0;
        if (const auto given = vm["retention-period"].as<std::string>(); !parseRetentionPeriod(given, seconds)) {
            std::cerr << "error: --retention-period has to be a number of seconds, a duration like 14d, 36h or 90m, "
                         "or -1 ('forever') to keep messages indefinitely, and was '"
                      << given << "'\n";
            return 1;
        }

        Dto::ENS::SetTopicRetentionRequest request;
        request.ern = vm["topic"].as<std::string>();
        request.retentionPeriod = seconds;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "set-topic-retention", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: set-topic-retention failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::setTopicMaxMessageLength(const std::vector<std::string> &args) const {
        po::options_description desc("set topic max message length options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace")
                ("max-length,m", po::value<std::string>()->required(),
                 "the largest message the topic accepts: bytes, or a size such as 512k, 2M or 1G");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "set-topic-max-message-length", "--topic <name|ern> --max-length <size>",
                                   "Sets the largest message a topic accepts, which create-topic set to 1M and which "
                                   "this is the only way to change afterwards. A publish-message whose body is larger "
                                   "is refused with HTTP 400, naming both the size sent and the limit; the body alone "
                                   "is measured, which is the same number get-topic-metadata reports as 'size'. "
                                   "It applies to what is published from here on: a message already in the topic was "
                                   "accepted under the rule in force when it arrived, and lowering the limit neither "
                                   "re-checks nor removes it. "
                                   "The size has to be positive - a topic that accepts nothing is a mistake rather than "
                                   "a configuration, and stop-topic is what says 'take nothing for now', reversibly.",
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

        long bytes = 0;
        if (const auto given = vm["max-length"].as<std::string>(); !parseByteSize(given, bytes)) {
            std::cerr << "error: --max-length has to be a positive number of bytes or a size like 512k, 2M or 1G, and was '"
                      << given << "'\n";
            return 1;
        }

        Dto::ENS::SetTopicMaxMessageLengthRequest request;
        request.ern = vm["topic"].as<std::string>();
        request.maxMessageLength = bytes;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "set-topic-max-message-length", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: set-topic-max-message-length failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::setTopicDelivering(const std::vector<std::string> &args, const bool delivering) const {

        const std::string action = delivering ? "start-topic" : "stop-topic";

        po::options_description desc(action + " options");
        desc.add_options()("topic,t", po::value<std::string>()->required(),
                           "topic name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return delivering
                           ? PrintActionHelp("ens", action, "--topic <name|ern>",
                                             "Starts delivering to a topic's subscribers again, and hands over what the topic "
                                             "held while it was stopped - oldest first, with the payload, attributes and "
                                             "priority each message was published with. Says how many that was. "
                                             "A topic that was already delivering is left as it is and releases nothing.",
                                             desc)
                           : PrintActionHelp("ens", action, "--topic <name|ern>",
                                             "Stops delivering to a topic's subscribers. Publishing carries on and the "
                                             "messages are stored as usual - they are simply held rather than handed on, and "
                                             "'ens start-topic' delivers them when the subscriber is ready for them. "
                                             "A held message expires like any other, so a topic left stopped for longer than "
                                             "its retention period loses what it was holding.",
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

        const boost::json::object request{{"ern", vm["topic"].as<std::string>()}};

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", action, request);
            if (!response.IsSuccess()) {
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

    int EnsCli::deleteTopicTag(const std::vector<std::string> &args) const {
        po::options_description desc("delete topic tag options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace")
                ("key,k", po::value<std::string>()->required(), "tag key");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "delete-topic-tag", "--topic <name|ern> --key <value>",
                                   "Deletes a tag from a topic.",
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

        Dto::ENS::DeleteTopicTagRequest request;
        request.ern = vm["topic"].as<std::string>();
        request.key = vm["key"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("ens", "delete-topic-tag", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: delete-topic-tag failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::subscribe(const std::vector<std::string> &args) const {
        po::options_description desc("subscribe options");
        desc.add_options()
                ("source-ern,s", po::value<std::string>()->required(), "source topic ERN")
                ("type,t", po::value<std::string>()->default_value("SQS"), "subscription type (only SQS is supported for now)")
                ("target-ern,q", po::value<std::string>()->required(), "target ERN (an EQS queue ERN)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "subscribe", "--source-ern <topicErn> --target-ern <queueErn> [--type SQS]",
                                   "Subscribes a target resource to a topic, so messages published to the topic are also "
                                   "delivered to the target. Only type SQS is supported for now, so --target-ern must be "
                                   "the ERN of an EQS queue; --type defaults to SQS.",
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

        Dto::ENS::SubscribeRequest request;
        request.sourceErn = vm["source-ern"].as<std::string>();
        request.type = vm["type"].as<std::string>();
        request.targetErn = vm["target-ern"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "subscribe", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: subscribe failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::unsubscribe(const std::vector<std::string> &args) const {
        po::options_description desc("unsubscribe options");
        desc.add_options()
                ("subscription,s", po::value<std::string>()->required(), "subscription ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "unsubscribe", "--subscription <ern>",
                                   "Deletes a subscription, identified by the ERN returned by euclid-cli-ens-subscribe(1). "
                                   "Deleting an ERN with no matching subscription is not an error.",
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

        Dto::ENS::UnsubscribeRequest request;
        request.ern = vm["subscription"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            if (const HttpResponse response = client.Post("ens", "unsubscribe", boost::json::value_from(request)); !response.IsSuccess()) {
                std::cerr << "error: unsubscribe failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EnsCli::listSubscriptions(const std::vector<std::string> &args) const {
        po::options_description desc("list subscriptions options");
        desc.add_options()
                ("topic,t", po::value<std::string>()->required(), "topic name; a full ERN also works and is what reaches another namespace");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ens", "list-subscriptions", "--topic <name|ern>",
                                   "Lists the subscriptions of a topic, identified by its Euclid resource name (ERN).",
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

        Dto::ENS::ListSubscriptionsRequest request;
        request.topicErn = vm["topic"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ens", "list-subscriptions", boost::json::value_from(request));
            if (!response.IsSuccess()) {
                std::cerr << "error: list-subscriptions failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
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