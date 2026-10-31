// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Euclid includes
#include <euclid/cli/emo/EmoCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    EmoCli::EmoCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    const std::vector<std::pair<std::string, std::string> > &EmoCli::Actions() {
        static const std::vector<std::pair<std::string, std::string> > kActions = {
                {"create-alert-rule", "Create a threshold rule on a metric"},
                {"delete-alert-rule", "Delete a threshold rule, clearing what it raised"},
                {"list-alert-history", "Show the alert state changes, newest first"},
                {"list-alert-rules", "List the threshold rules"},
                {"list-alerts", "Show where every series a rule judges stands now"},
                {"update-alert-rule", "Change a threshold rule"},
        };
        return kActions;
    }

    int EmoCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("emo", Actions());
        }

        if (!IsHelpRequest(args) && _authentication.token.empty()) {
            std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
            return 1;
        }

        if (action == "create-alert-rule") return createAlertRule(args);
        if (action == "update-alert-rule") return updateAlertRule(args);
        if (action == "delete-alert-rule") return deleteAlertRule(args);
        if (action == "list-alert-rules") return listAlertRules(args);
        if (action == "list-alerts") return listAlerts(args);
        if (action == "list-alert-history") return listAlertHistory(args);

        std::cerr << "error: unknown emo action '" << action << "'\n";
        return 1;
    }

    int EmoCli::send(const std::string &action, const boost::json::object &request) const {
        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emo", action, request);
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

    namespace {

        bool parse(const std::vector<std::string> &args, const po::options_description &desc, po::variables_map &vm) {
            try {
                po::store(po::command_line_parser(args).options(desc).run(), vm);
                po::notify(vm);
                return true;
            } catch (const po::error &ex) {
                std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
                return false;
            }
        }

        // The fields a create and an update share. Every one optional here: a create needs the
        // metric and a threshold, which the server says when they are missing.
        void addRuleOptions(po::options_description &desc) {
            desc.add_options()
                    ("metric,m", po::value<std::string>(), "the metric to judge, e.g. system-load-per-core")
                    ("label,l", po::value<std::vector<std::string> >()->composing(), "only series with this label, as name=value; repeatable")
                    ("statistic", po::value<std::string>(), "which figure of a period: value (the mean, or the sum for a rate), max or min")
                    ("comparison", po::value<std::string>(), "above or below: which side of the thresholds is the wrong one")
                    ("warning,w", po::value<double>(), "warning threshold")
                    ("critical,c", po::value<double>(), "critical threshold")
                    ("for-periods", po::value<long>(), "periods past a threshold before the state is entered")
                    ("clear-periods", po::value<long>(), "periods back inside before it is left")
                    ("no-data-after-periods", po::value<long>(), "periods without data before NO_DATA; 0 never")
                    ("description,d", po::value<std::string>(), "what the rule is for");
        }

        // Only what was given, so an update changes exactly that.
        std::string addRuleFields(boost::json::object &request, const po::variables_map &vm) {
            if (vm.contains("metric")) request["name"] = vm["metric"].as<std::string>();
            if (vm.contains("label")) {
                boost::json::object labels;
                for (const auto &label: vm["label"].as<std::vector<std::string> >()) {
                    const auto equals = label.find('=');
                    if (equals == std::string::npos || equals == 0) return "--label must be name=value, got '" + label + "'";
                    labels[label.substr(0, equals)] = label.substr(equals + 1);
                }
                request["labels"] = std::move(labels);
            }
            if (vm.contains("statistic")) request["statistic"] = vm["statistic"].as<std::string>();
            if (vm.contains("comparison")) request["comparison"] = vm["comparison"].as<std::string>();
            if (vm.contains("warning")) request["warning"] = vm["warning"].as<double>();
            if (vm.contains("critical")) request["critical"] = vm["critical"].as<double>();
            if (vm.contains("for-periods")) request["forPeriods"] = vm["for-periods"].as<long>();
            if (vm.contains("clear-periods")) request["clearPeriods"] = vm["clear-periods"].as<long>();
            if (vm.contains("no-data-after-periods")) request["noDataAfterPeriods"] = vm["no-data-after-periods"].as<long>();
            if (vm.contains("description")) request["description"] = vm["description"].as<std::string>();
            return {};
        }

    }// namespace

    int EmoCli::createAlertRule(const std::vector<std::string> &args) const {

        po::options_description desc("create alert rule options");
        desc.add_options()("rule-id,r", po::value<std::string>()->required(), "the rule's name, unique in the account");
        addRuleOptions(desc);
        desc.add_options()("disabled", "store the rule without evaluating it yet");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emo", "create-alert-rule",
                                   "--rule-id <id> --metric <name> [--label <name=value>]... [--statistic value|max|min] "
                                   "[--comparison above|below] [--warning <n>] [--critical <n>] [--for-periods <n>] "
                                   "[--clear-periods <n>] [--no-data-after-periods <n>] [--description <text>] [--disabled]",
                                   "Creates a threshold rule on one metric. Every period EMO writes the metric's rows - five "
                                   "minutes as shipped - and judges each series the rule's labels match: past --warning or "
                                   "--critical for --for-periods periods running and the series is WARNING or CRITICAL, back "
                                   "inside for --clear-periods and it is OK again. Each change is published on the event bus as "
                                   "emo.alert.raised or emo.alert.cleared, which anything can subscribe to through EES, and "
                                   "recorded for list-alert-history. With --no-data-after-periods a series that stops arriving "
                                   "goes NO_DATA. --statistic decides which figure is judged: value is the period's mean (its "
                                   "sum for a rate), so a short spike only shows in max. Defaults: value, above, one period "
                                   "each way, no NO_DATA. A rule matching more series than "
                                   "euclid.modules.emo.alert.max-series-per-rule is refused - narrow it with --label.",
                                   desc);
        }

        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;

        boost::json::object request{{"ruleId", vm["rule-id"].as<std::string>()}};
        if (auto error = addRuleFields(request, vm); !error.empty()) {
            std::cerr << "error: " << error << std::endl;
            return 1;
        }
        if (vm.contains("disabled")) request["enabled"] = false;
        return send("create-alert-rule", request);
    }

    int EmoCli::updateAlertRule(const std::vector<std::string> &args) const {

        po::options_description desc("update alert rule options");
        desc.add_options()("rule-id,r", po::value<std::string>()->required(), "the rule to change");
        addRuleOptions(desc);
        desc.add_options()
                ("no-warning", "remove the warning threshold")
                ("no-critical", "remove the critical threshold")
                ("no-labels", "judge every series of the metric")
                ("enable", "evaluate the rule")
                ("disable", "stop evaluating the rule, clearing what it raised");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emo", "update-alert-rule",
                                   "--rule-id <id> [any create-alert-rule option] [--no-warning] [--no-critical] [--no-labels] "
                                   "[--enable | --disable]",
                                   "Changes only what is given. A changed threshold applies from the next period, to the series "
                                   "as they stand. A changed metric or label selector, or --disable, ends the series the rule "
                                   "was judging: each one not OK is cleared, published as emo.alert.cleared with the reason, "
                                   "so nobody waits on an alert the rule can no longer clear. --label replaces the whole "
                                   "selector.",
                                   desc);
        }

        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        if (vm.contains("enable") && vm.contains("disable")) {
            std::cerr << "error: --enable and --disable are each other's opposite" << std::endl;
            return 1;
        }

        boost::json::object request{{"ruleId", vm["rule-id"].as<std::string>()}};
        if (auto error = addRuleFields(request, vm); !error.empty()) {
            std::cerr << "error: " << error << std::endl;
            return 1;
        }
        if (vm.contains("no-warning")) request["warning"] = nullptr;
        if (vm.contains("no-critical")) request["critical"] = nullptr;
        if (vm.contains("no-labels")) request["labels"] = boost::json::object{};
        if (vm.contains("enable")) request["enabled"] = true;
        if (vm.contains("disable")) request["enabled"] = false;
        return send("update-alert-rule", request);
    }

    int EmoCli::deleteAlertRule(const std::vector<std::string> &args) const {

        po::options_description desc("delete alert rule options");
        desc.add_options()("rule-id,r", po::value<std::string>()->required(), "the rule to delete");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emo", "delete-alert-rule", "--rule-id <id>",
                                   "Deletes a rule and the state of every series it judged. Each series not OK is cleared "
                                   "first and published as emo.alert.cleared with reason rule-deleted. Its history stays until "
                                   "it expires.",
                                   desc);
        }

        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        return send("delete-alert-rule", boost::json::object{{"ruleId", vm["rule-id"].as<std::string>()}});
    }

    int EmoCli::listAlertRules(const std::vector<std::string> &args) const {

        po::options_description desc("list alert rules options");
        desc.add_options()("rule-id,r", po::value<std::string>(), "only this rule");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emo", "list-alert-rules", "[--rule-id <id>]", "Lists the account's threshold rules.", desc);
        }

        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        boost::json::object request;
        if (vm.contains("rule-id")) request["ruleId"] = vm["rule-id"].as<std::string>();
        return send("list-alert-rules", request);
    }

    int EmoCli::listAlerts(const std::vector<std::string> &args) const {

        po::options_description desc("list alerts options");
        desc.add_options()
                ("rule-id,r", po::value<std::string>(), "only this rule's series")
                ("state,s", po::value<std::string>(), "only series in this state: OK, WARNING, CRITICAL or NO_DATA")
                ("active,a", "only series that are not OK");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emo", "list-alerts", "[--rule-id <id>] [--state <state>] [--active]",
                                   "Shows where every series a rule judges stands now - its state, since when, the last figure "
                                   "judged and the lines it was judged against. A series a rule has not seen a row of yet is "
                                   "not listed.",
                                   desc);
        }

        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        boost::json::object request;
        if (vm.contains("rule-id")) request["ruleId"] = vm["rule-id"].as<std::string>();
        if (vm.contains("state")) request["state"] = vm["state"].as<std::string>();
        if (vm.contains("active")) request["active"] = true;
        return send("list-alerts", request);
    }

    int EmoCli::listAlertHistory(const std::vector<std::string> &args) const {

        po::options_description desc("list alert history options");
        desc.add_options()
                ("rule-id,r", po::value<std::string>(), "only this rule's changes")
                ("from", po::value<std::string>(), "from this time, ISO 8601")
                ("to", po::value<std::string>(), "until this time, ISO 8601")
                ("limit,n", po::value<long>()->default_value(100), "how many");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emo", "list-alert-history", "[--rule-id <id>] [--from <time>] [--to <time>] [--limit <n>]",
                                   "Shows the alert state changes, newest first: which series, from what to what, the figure "
                                   "and the line that decided it. Kept for euclid.modules.emo.alert.history-retention days, "
                                   "90 as shipped.",
                                   desc);
        }

        po::variables_map vm;
        if (!parse(args, desc, vm)) return 1;
        boost::json::object request{{"limit", vm["limit"].as<long>()}};
        if (vm.contains("rule-id")) request["ruleId"] = vm["rule-id"].as<std::string>();
        if (vm.contains("from")) request["from"] = vm["from"].as<std::string>();
        if (vm.contains("to")) request["to"] = vm["to"].as<std::string>();
        return send("list-alert-history", request);
    }

}// namespace Euclid::CLI
