// C++ includes
#include <sstream>

// Euclid includes
#include <euclid/cli/ees/EesCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Splits a comma-separated option value into trimmed, non-empty entries, e.g.
        // "esm.object.created, esm.object.deleted" -> two entries. Used for --event-types and
        // --event-ids, which are lists on the wire but one argument on the command line.
        boost::json::array SplitList(const std::string &value) {
            boost::json::array entries;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ','); ) {
                const auto first = part.find_first_not_of(" \t");
                const auto last = part.find_last_not_of(" \t");
                if (first == std::string::npos) continue;
                entries.push_back(boost::json::string(part.substr(first, last - first + 1)));
            }
            return entries;
        }

    }// namespace

    EesCli::EesCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EesCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ees", {
                                           {"subscribe-events", "Subscribe a name to one or more event types"},
                                           {"unsubscribe-events", "Remove a subscription and the events waiting for it"},
                                           {"list-subscriptions", "Show a subscriber's subscriptions and backlog"},
                                           {"receive-events", "Claim a subscriber's waiting events"},
                                           {"ack-events", "Acknowledge claimed events, deleting them"},
                                   });
        }

        // Authenticated, but deliberately not administrator-only, unlike ets or eap: a subscriber
        // only ever sees events of its own account, and the principal that needs to subscribe is
        // usually an application's own technical user rather than an operator. Checked here for a
        // clearer message than the 401 the gateway would return; the server checks regardless.
        if (!IsHelpRequest(args) && _authentication.token.empty()) {
            std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
            return 1;
        }

        if (action == "subscribe-events") return subscribeEvents(args);
        if (action == "unsubscribe-events") return unsubscribeEvents(args);
        if (action == "list-subscriptions") return listSubscriptions(args);
        if (action == "receive-events") return receiveEvents(args);
        if (action == "ack-events") return ackEvents(args);

        std::cerr << "error: unknown ees action '" << action << "'\n";
        return 1;
    }

    int EesCli::subscribeEvents(const std::vector<std::string> &args) const {
        po::options_description desc("subscribe to events");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "subscriber name events are stored and claimed under")
                ("event-types,e", po::value<std::string>()->required(), "comma-separated event types, e.g. esm.object.created,esm.object.deleted")
                ("filter,f", po::value<std::string>(), "JSON object of payload fields that must match exactly, e.g. '{\"bucketName\":\"inbox\"}'")
                ("mode,m", po::value<std::string>()->default_value("durable"), "durable (events are kept until acknowledged) or live (pushed to connected sessions only)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ees", "subscribe-events",
                                   "--name <name> --event-types <list> [--filter <json>] [--mode durable|live]",
                                   "Registers a durable subscription. From then on, every matching event is stored "
                                   "for this name whether or not anything is connected, and stays there until it is "
                                   "acknowledged - so an application misses nothing while it restarts. Two consumers "
                                   "using different names each get their own copy of an event; two instances using the "
                                   "same name share the work, since a claim is atomic and only one of them gets any "
                                   "given event. "
                                   "The filter is applied when the event is published rather than when it is "
                                   "delivered, so a backlog is proportional to what was asked for rather than to how "
                                   "busy the installation is; ESM's object events carry bucketName, key, prefix and "
                                   "directory for exactly this purpose. Subscribing again with the same name and event "
                                   "type replaces the filter rather than adding a second subscription. "
                                   "An unclaimed event is kept for seven days. "
                                   "A live subscription stores nothing at all: its events are pushed to whatever "
                                   "websocket sessions are attached to the name and are then gone, which is what a "
                                   "view wants - a screen showing a bucket has no use for the hour of events it "
                                   "missed while nobody was looking at it.",
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

        const auto eventTypes = SplitList(vm["event-types"].as<std::string>());
        if (eventTypes.empty()) {
            std::cerr << "error: subscribe-events failed: --event-types named no event type\n";
            return 1;
        }

        const auto mode = vm["mode"].as<std::string>();
        if (mode != "durable" && mode != "live") {
            std::cerr << "error: subscribe-events failed: --mode must be \"durable\" or \"live\"\n";
            return 1;
        }

        boost::json::object request{
                {"name", vm["name"].as<std::string>()},
                {"eventTypes", eventTypes},
                {"mode", mode}
        };

        // Parsed here rather than passed through as a string, so a mistyped filter is a message
        // about the filter instead of a subscription that silently matches everything.
        if (vm.contains("filter")) {
            try {
                const auto parsed = boost::json::parse(vm["filter"].as<std::string>());
                if (!parsed.is_object()) {
                    std::cerr << "error: subscribe-events failed: --filter must be a JSON object, e.g. '{\"bucketName\":\"inbox\"}'\n";
                    return 1;
                }
                request["filter"] = parsed.as_object();
            } catch (const std::exception &) {
                // Boost's own message carries a parser location and a build path, neither of which
                // helps with a filter typed on a command line.
                std::cerr << "error: subscribe-events failed: --filter is not valid JSON, e.g. '{\"bucketName\":\"inbox\"}'\n";
                return 1;
            }
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ees", "subscribe-events", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: subscribe-events failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EesCli::unsubscribeEvents(const std::vector<std::string> &args) const {
        po::options_description desc("remove a subscription");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "subscriber name")
                ("event-type,e", po::value<std::string>(), "event type to stop receiving; omit to remove all of this subscriber's subscriptions");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ees", "unsubscribe-events", "--name <name> [--event-type <type>]",
                                   "Removes a subscription, and every event still waiting for it - keeping a backlog "
                                   "for something nobody is listening to any more is what the seven-day retention "
                                   "exists to prevent, and this does it now rather than a week from now. "
                                   "Naming no event type removes the subscriber entirely, which is what "
                                   "decommissioning an application wants; naming one narrows it and leaves the rest "
                                   "of its subscriptions alone.",
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

        boost::json::object request{{"name", vm["name"].as<std::string>()}};
        if (vm.contains("event-type")) request["eventType"] = vm["event-type"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ees", "unsubscribe-events", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: unsubscribe-events failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EesCli::listSubscriptions(const std::vector<std::string> &args) const {
        po::options_description desc("list subscriptions");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "subscriber name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ees", "list-subscriptions", "--name <name>",
                                   "Shows what a subscriber is subscribed to - its event types, filters and account - "
                                   "together with \"waiting\", the number of events currently held for it, claimed or "
                                   "not. A backlog that only grows is the sign of a consumer that receives without "
                                   "acknowledging, or one that stopped running while its subscription stayed. "
                                   "\"lastSeen\" is when it last claimed anything.",
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ees", "list-subscriptions", boost::json::object{{"name", vm["name"].as<std::string>()}});
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

    int EesCli::receiveEvents(const std::vector<std::string> &args) const {
        po::options_description desc("receive events");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "subscriber name to claim events for")
                ("max-events,m", po::value<long>()->default_value(10), "largest number of events to claim")
                ("wait-time,w", po::value<long>()->default_value(0), "seconds to wait for an event before answering with none, up to 20")
                ("visibility-timeout,V", po::value<long>(), "seconds the claim holds before the events become claimable again (default 300)")
                ("acknowledge,a", po::bool_switch(), "acknowledge the received events immediately, deleting them");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ees", "receive-events",
                                   "--name <name> [--max-events <n>] [--wait-time <seconds>] "
                                   "[--visibility-timeout <seconds>] [--acknowledge]",
                                   "Claims up to --max-events of a subscriber's waiting events. A claim is a lease, "
                                   "not a delete: the events are invisible to any other claimer until the visibility "
                                   "timeout runs out, and come back if they are never acknowledged - which is what "
                                   "makes a consumer crashing mid-work harmless. Acknowledge them with \"ees "
                                   "ack-events\" once the work is actually done, or pass --acknowledge here for the "
                                   "cases where reading them is the work. "
                                   "Without --acknowledge, events received at a terminal reappear after the "
                                   "visibility timeout, so an unacknowledged look at a backlog costs nothing. Note "
                                   "that claiming under an application's subscriber name takes those events from it: "
                                   "use a name of your own to watch a stream alongside it.",
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

        const auto subscriber = vm["name"].as<std::string>();

        boost::json::object request{
                {"name", subscriber},
                {"maxEvents", vm["max-events"].as<long>()},
                {"waitTime", vm["wait-time"].as<long>()}
        };
        if (vm.contains("visibility-timeout")) request["visibilityTimeout"] = vm["visibility-timeout"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ees", "receive-events", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: receive-events failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);

            if (!vm["acknowledge"].as<bool>()) return 0;

            // Printed before acknowledging, so the events are on screen even if the ack fails -
            // the events themselves are the point of the command, and an ack that did not happen
            // costs a redelivery rather than the data.
            boost::json::array eventIds;
            if (response.body.is_object()) {
                if (const auto *events = response.body.as_object().if_contains("events"); events && events->is_array()) {
                    for (const auto &event: events->as_array()) {
                        if (event.is_object()) {
                            if (const auto *eventId = event.as_object().if_contains("eventId"); eventId && eventId->is_string()) {
                                eventIds.push_back(*eventId);
                            }
                        }
                    }
                }
            }
            if (eventIds.empty()) return 0;

            const HttpResponse ack = client.Post("ees", "ack-events",
                                                 boost::json::object{{"name", subscriber}, {"eventIds", eventIds}});
            if (!ack.IsSuccess()) {
                std::cerr << "warning: events were received but not acknowledged (HTTP " << ack.statusCode << "): "
                        << boost::json::serialize(ack.body) << "; they will be delivered again once the visibility timeout runs out" << std::endl;
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EesCli::ackEvents(const std::vector<std::string> &args) const {
        po::options_description desc("acknowledge events");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "subscriber name the events were claimed under")
                ("event-ids,i", po::value<std::string>()->required(), "comma-separated event IDs from a receive-events answer");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ees", "ack-events", "--name <name> --event-ids <list>",
                                   "Deletes claimed events, which is what \"processed\" means here: until an event is "
                                   "acknowledged it is only leased, and comes back when the lease runs out. "
                                   "An event that is not there any more is not an error - a redelivery acknowledged "
                                   "twice and one whose retention ran out both mean the same thing to a caller - so "
                                   "the answer reports how many were actually deleted, and how many are still waiting.",
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

        // Caught here because an empty list is what an unset shell variable expands to, and the
        // 400 that would come back says nothing about which argument produced it.
        const auto eventIds = SplitList(vm["event-ids"].as<std::string>());
        if (eventIds.empty()) {
            std::cerr << "error: ack-events failed: --event-ids named no event ID\n";
            return 1;
        }

        const boost::json::object request{
                {"name", vm["name"].as<std::string>()},
                {"eventIds", eventIds}
        };

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ees", "ack-events", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: ack-events failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
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
