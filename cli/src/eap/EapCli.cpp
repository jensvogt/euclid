// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <filesystem>
#include <map>
#include <sstream>

// Euclid includes
#include <euclid/database/entity/eap/Infrastructure.h>
#include <fstream>
#include <algorithm>
#include <euclid/cli/eap/EapCli.h>
#include <euclid/cli/esm/EsmCli.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/database/entity/eap/Application.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Splits "KEY=value,OTHER=value" into a JSON object. Environment variables are a map on
        // the wire but a single argument on the command line, and only the first '=' separates
        // name from value - the value may well contain more of them.
        boost::json::object SplitEnvironment(const std::string &value) {
            boost::json::object environment;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ',');) {
                const auto first = part.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                const auto equals = part.find('=', first);
                if (equals == std::string::npos) continue;
                environment[part.substr(first, equals - first)] = part.substr(equals + 1);
            }
            return environment;
        }

        boost::json::array SplitList(const std::string &value) {
            boost::json::array entries;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ',');) {
                const auto first = part.find_first_not_of(" \t");
                const auto last = part.find_last_not_of(" \t");
                if (first == std::string::npos) continue;
                entries.push_back(boost::json::string(part.substr(first, last - first + 1)));
            }
            return entries;
        }

        // Splits "os=windows,gpu" into the nodeLabels object placement matches against. A bare
        // label means "true", as it does in the worker's own --label, so "gpu" asks for the node
        // that was labelled "gpu". An empty string gives an empty object, which clears them.
        boost::json::object SplitLabels(const std::string &value) {
            boost::json::object labels;
            for (const auto &entry: SplitList(value)) {
                const std::string part(entry.as_string());
                const auto equals = part.find('=');
                if (equals == std::string::npos) labels[part] = "true";
                else labels[part.substr(0, equals)] = part.substr(equals + 1);
            }
            return labels;
        }

        // The objects a manifest names, split the way create-application and update-application
        // take them. Both halves count: an application reaches what it owns and what it borrows,
        // and a grant that named only the first would refuse it the second.
        //
        // This is what turns a manifest into a narrower principal. Without it EAP grants the
        // application role with resources = ["*"], because a deployment that says nothing about
        // what it needs can only safely be read as needing everything in its namespace.
        bool addManifestResources(const std::filesystem::path &directory, boost::json::object &request) {

            const auto loaded = Database::Entity::EAP::Infrastructure::LoadFolder(directory);
            if (!loaded.error.empty()) {
                std::cerr << "error: " << directory.string() << " cannot be read: " << loaded.error << "\n";
                return false;
            }

            boost::json::array buckets;
            boost::json::array queues;
            boost::json::array topics;
            boost::json::array secrets;

            // Named for the same reason as the other three, secrets included: the application role
            // holds ess:get-secret and that action is resource-checked, so a deployment that did
            // not name its secrets would reach every secret in the namespace.
            // Tables and routes are not among them: a table is granted by EAP from the folder itself,
            // and a route is reached over HTTP rather than through a permission.
            const auto add = [&](const Database::Entity::EAP::Infrastructure::Resource &resource) {
                if (resource.kind == "buckets") buckets.push_back(boost::json::string(resource.name));
                else if (resource.kind == "queues") queues.push_back(boost::json::string(resource.name));
                else if (resource.kind == "topics") topics.push_back(boost::json::string(resource.name));
                else if (resource.kind == "secrets") secrets.push_back(boost::json::string(resource.name));
            };
            for (const auto &resource: loaded.declaration.creates) add(resource);
            for (const auto &resource: loaded.declaration.uses) add(resource);

            if (buckets.empty() && queues.empty() && topics.empty() && secrets.empty()) {
                // A manifest that declares nothing would otherwise send four empty lists, and EAP
                // reads "no resources named" as "every resource in the account" - the opposite of
                // what a manifest is for. Better to leave the deployment's own lists alone.
                std::cerr << "warning: " << directory.string() << " declares no objects; the deployment's resources are unchanged\n";
                return true;
            }

            request["buckets"] = buckets;
            request["queues"] = queues;
            request["topics"] = topics;
            request["secrets"] = secrets;
            return true;
        }
    }// namespace

    EapCli::EapCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    /**
     * @brief Every action this module takes, with the one-line summary each is listed by.
     *
     * @par
     * A table rather than an initialiser inside the help branch, because two things read
     * it now: "help", and tab completion. An action listed in one and not the other is an
     * action somebody cannot find.
     */
    const std::vector<std::pair<std::string, std::string> > &EapCli::Actions() {
        static const std::vector<std::pair<std::string, std::string> > kActions = {
                {"apply", "Make the installation match an application's euclid/ folder: objects, settings and access"},
                {"copy-application", "Define the same application again in another namespace"},
                {"create-application", "Define a new application from an artifact in an ESM bucket"},
                {"delete-application", "Delete an application definition"},
                {"delete-egress-limit", "Remove an application's egress limit for an outside site"},
                {"delete-egress-site", "Forget an outside site of an application: its limit, bucket and slots"},
                {"delete-node", "Remove a worker node's registration, freeing its name"},
                {"deploy-infrastructure", "The same as apply, under its older name"},
                {"get-platform", "Show a platform declaration: what it owns, and the declaration last applied"},
                {"list-platforms", "List the platform declarations of the namespace"},
                {"get-application", "Show one application's definition"},
                {"get-node", "Show one worker node's registration"},
                {"list-egress-sites", "List each application's outside sites, egress limits and how they stand"},
                {"list-applications", "List the defined applications and how many instances are running"},
                {"list-nodes", "List the registered worker nodes and whether they are live"},
                {"redeploy-application", "Deploy a new build of an application from a local file"},
                {"restart-application", "Ask the manager to start an application's instances again"},
                {"rebalance-application", "Ask the manager to spread an application's instances over the nodes again"},
                {"set-egress-limit", "Set how fast one application may call an outside site"},
                {"set-log-level", "Turn an application's own logging up, down or off"},
                {"scale-application", "Change how many instances an application runs, without restarting it"},
                {"start-application", "Ask the manager to start an application"},
                {"stop-application", "Ask the manager to stop an application"},
                {"update-application", "Change an existing application's definition"},
        };
        return kActions;
    }

    int EapCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("eap", Actions());
        }

        // Every eap action decides which code euclid executes and under whose identity, so all of
        // them are administrator-only. Checked here, once, ahead of dispatch - a client-side
        // fail-fast for a better error message only, not the security boundary: the server
        // re-checks every request regardless of what an older CLI binary sends. Help is exempt,
        // since usage text gives nothing away.
        if (!IsHelpRequest(args)) {
            if (_authentication.token.empty()) {
                std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
                return 1;
            }
            if (!_authentication.isAdmin) {
                std::cerr << "error: " << action << " failed: administrator privileges required\n";
                return 1;
            }
        }

        if (action == "create-application") return createApplication(args);
        if (action == "update-application") return updateApplication(args);
        if (action == "copy-application") return copyApplication(args);
        if (action == "scale-application") return scaleApplication(args);
        if (action == "deploy-infrastructure") return deployInfrastructure(args);
        if (action == "get-platform" || action == "list-platforms") return platformInfo(action, args);
        if (action == "redeploy-application") return redeployApplication(args);
        if (action == "list-applications") return listApplications(args);
        if (action == "get-application") return getApplication(args);
        if (action == "delete-application") return deleteApplication(args);
        if (action == "start-application") return setState(args, true);
        if (action == "stop-application") return setState(args, false);
        if (action == "restart-application") return restartApplication(args);
        if (action == "rebalance-application") return rebalanceApplication(args);
        if (action == "set-log-level") return setLogLevel(args);
        if (action == "apply") return applyManifest(args);
        if (action == "list-nodes") return listNodes(args);
        if (action == "get-node") return getNode(args);
        if (action == "delete-node") return deleteNode(args);
        if (action == "list-egress-sites") return listEgressSites(args);
        if (action == "set-egress-limit") return setEgressLimit(args);
        if (action == "delete-egress-limit") return deleteEgressLimit(args);
        if (action == "delete-egress-site") return deleteEgressSite(args);

        std::cerr << "error: unknown eap action '" << action << "'\n";
        return 1;
    }

    int EapCli::createApplication(const std::vector<std::string> &args) const {
        po::options_description desc("create a new application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name identifying the application, unique across the installation")
                ("runtime,r", po::value<std::string>()->required(), "runtime the artifact is started with: JAVA, JAVA21, JAVA25, PYTHON, NODEJS or BINARY")
                ("bucket,b", po::value<std::string>()->required(), "name of the ESM bucket holding the artifact")
                ("artifact,a", po::value<std::string>()->required(), "object key of the artifact within that bucket")
                ("version", po::value<std::string>(), "version of this build; read out of the artifact name (x.y.z) when not given")
                ("user,u", po::value<std::string>(), "EAM user the application runs as; defaults to a technical principal created for this application alone")
                ("buckets", po::value<std::string>(), "comma-separated names of the ESM buckets the application may use; empty means every bucket in its account")
                ("manifest", po::value<std::string>(), "an application euclid/ directory; the objects it declares become the resources this application is granted, instead of every resource in its namespace")
                ("queues", po::value<std::string>(), "comma-separated names of the EQS queues the application may use; empty means every queue in its account")
                ("command,c", po::value<std::string>(), "command to run instead of the runtime's default")
                ("arguments", po::value<std::string>(), "comma-separated arguments passed after the artifact")
                ("environment,e", po::value<std::string>(), "comma-separated KEY=value environment variables")
                ("node-labels", po::value<std::string>(), "comma-separated key=value labels a worker node must carry to run it, e.g. os=windows; empty means any node")
                ("min-instances", po::value<long>(), "smallest number of instances the autoscaler keeps running (default 1)")
                ("max-instances", po::value<long>(), "largest number of instances the autoscaler may scale out to (default 1)")
                ("type", po::value<std::string>(), "PROCESS (default) for something that stays up, or JOB for something that runs once and finishes: a JOB is not restarted when it exits 0, and is not autoscaled")
                ("schedule", po::value<std::string>(), "cron expression a JOB runs on, in UTC, e.g. \"0 2 * * *\" or \"@daily\"; only valid with --type JOB, and an empty value unschedules it")
                ("ready-timeout", po::value<long>(), "kept on the definition but no longer decides readiness: an application counts as started by surviving its own startup, not by creating a socket");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "create-application",
                                   "--application-id <name> --runtime <runtime> --bucket <bucket> --artifact <key> [--version <x.y.z>] "
                                   "[--user <user>] [--buckets <list>] [--queues <list>] [--command <cmd>] "
                                   "[--arguments <list>] [--environment <list>] [--node-labels <list>] [--min-instances <n>] "
                                   "[--max-instances <n>] [--ready-timeout <ms>]",
                                   "Defines a new application from an artifact already stored in an ESM bucket - upload it first with "
                                   "\"esm upload-file\", or through a transfer server. The manager copies the artifact to the host, "
                                   "starts it with the runtime's interpreter (java -jar, python3, node) or directly for BINARY, and "
                                   "scales it between --min-instances and --max-instances like any other module. An instance counts as "
                                   "started once it is still running a moment after it was spawned, so an application that only does "
                                   "work of its own is never killed for failing to answer a convention it was not written for; a socket "
                                   "path is still handed to it in EUCLID_SOCKET for one that does want to serve x-euclid-action requests. "
                                   "Its endpoint and credentials arrive the same way - EUCLID_BASE_URL, and a rotating token in the file "
                                   "named by EUCLID_CREDENTIALS_FILE - which it uses to sign its own calls back into euclid (RFC 9421). "
                                   "Without --user the application gets "
                                   "a technical principal of its own (\"app-<application-id>\"): an EAM identity that cannot log in and "
                                   "has nothing but that key, created here and deleted with the application, so no application ever runs "
                                   "on a person's credentials. Name --user only when an application really should act as an existing user. "
                                   "Naming --buckets and --queues narrows that principal to exactly those resources: the storage and "
                                   "queueing modules refuse anything else it asks for, so a compromised application reaches what it was "
                                   "deployed with and nothing more. Naming neither leaves it able to use everything in its account. "
                                   "--node-labels restricts which worker nodes it may be placed on: every node carries the os "
                                   "(linux, windows, macos) and arch it reported, so --node-labels os=windows runs it on Windows "
                                   "workers only. "
                                   "The application is created stopped - use \"eap start-application\" to run it.",
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

        boost::json::object request{
                {"applicationId", vm["application-id"].as<std::string>()},
                {"runtime", vm["runtime"].as<std::string>()},
                {"bucket", vm["bucket"].as<std::string>()},
                {"artifact", vm["artifact"].as<std::string>()}
        };
        if (vm.contains("version")) request["version"] = vm["version"].as<std::string>();
        if (vm.contains("user")) request["user"] = vm["user"].as<std::string>();
        if (vm.contains("command")) request["command"] = vm["command"].as<std::string>();
        if (vm.contains("namespace")) request["namespace"] = vm["namespace"].as<std::string>();
        if (vm.contains("arguments")) request["arguments"] = SplitList(vm["arguments"].as<std::string>());
        if (vm.contains("environment")) request["environment"] = SplitEnvironment(vm["environment"].as<std::string>());
        if (vm.contains("buckets")) request["buckets"] = SplitList(vm["buckets"].as<std::string>());
        if (vm.contains("queues")) request["queues"] = SplitList(vm["queues"].as<std::string>());
        if (vm.contains("node-labels")) request["nodeLabels"] = SplitLabels(vm["node-labels"].as<std::string>());
        if (vm.contains("manifest") && !addManifestResources(vm["manifest"].as<std::string>(), request)) return 1;
        if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
        if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        if (vm.contains("type")) request["type"] = vm["type"].as<std::string>();
        if (vm.contains("schedule")) request["schedule"] = vm["schedule"].as<std::string>();
        if (vm.contains("ready-timeout")) request["readyTimeoutMs"] = vm["ready-timeout"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "create-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: create-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::updateApplication(const std::vector<std::string> &args) const {
        po::options_description desc("update an existing application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to change")
                ("runtime,r", po::value<std::string>(), "runtime the artifact is started with")
                ("artifact,a", po::value<std::string>(), "object key of the artifact within the application's bucket")
                ("command,c", po::value<std::string>(), "command to run instead of the runtime's default")
                ("user,u", po::value<std::string>(), "EAM user the application runs as; it has to exist and have an access key")
                ("namespace", po::value<std::string>(), "namespace the application's own requests run in, which is what lets it name a queue or topic rather than spell out a full ERN; pass an empty string to move it back to the account root")
                ("arguments", po::value<std::string>(), "comma-separated arguments; replaces the current list")
                ("environment,e", po::value<std::string>(), "comma-separated KEY=value environment variables; replaces the current set")
                ("buckets", po::value<std::string>(), "comma-separated bucket names the application may use; replaces the current list")
                ("manifest", po::value<std::string>(), "an application euclid/ directory; the objects it declares become the resources this application is granted, instead of every resource in its namespace")
                ("queues", po::value<std::string>(), "comma-separated queue names the application may use; replaces the current list")
                ("node-labels", po::value<std::string>(), "comma-separated key=value labels a worker node must carry to run it, e.g. os=windows; replaces the current set, and an empty string clears it")
                ("min-instances", po::value<long>(), "smallest number of instances the autoscaler keeps running")
                ("max-instances", po::value<long>(), "largest number of instances the autoscaler may scale out to")
                ("type", po::value<std::string>(), "PROCESS (default) for something that stays up, or JOB for something that runs once and finishes: a JOB is not restarted when it exits 0, and is not autoscaled")
                ("schedule", po::value<std::string>(), "cron expression a JOB runs on, in UTC, e.g. \"0 2 * * *\" or \"@daily\"; only valid with --type JOB, and an empty value unschedules it")
                ("ready-timeout", po::value<long>(), "kept on the definition but no longer decides readiness: an application counts as started by surviving its own startup, not by creating a socket");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "update-application",
                                   "--application-id <name> [--runtime <runtime>] [--artifact <key>] [--command <cmd>] "
                                   "[--arguments <list>] [--environment <list>] [--buckets <list>] [--queues <list>] "
                                   "[--node-labels <list>] [--min-instances <n>] [--max-instances <n>] [--ready-timeout <ms>] [--user <userId>]",
                                   "Changes an existing application's definition. Only the options actually given are altered, so one "
                                   "setting can be changed without resending the whole definition; --arguments and --environment "
                                   "replace the current values rather than adding to them. A running application is restarted onto the "
                                   "new definition by the manager's reconciler within a few seconds - an artifact, a command, an "
                                   "environment and the credentials a process was handed are all decided when it starts, so there is no "
                                   "other way to apply them. That also makes this the way to deploy a new build: upload it over the same "
                                   "key with \"esm upload-file\" and run this command, which re-materialises the artifact even when "
                                   "nothing about the definition changed. "
                                   "Changing --buckets or --queues re-grants the application's technical principal, so the "
                                   "new list takes effect immediately, for running instances too. "
                                   "--node-labels replaces the labels a worker node must carry to take the application - "
                                   "--node-labels os=windows for Windows workers only - and --node-labels \"\" lets it run on any node again. "
                                   "--user points the application at a different EAM identity, which has to exist and hold an access "
                                   "key - the repair for a definition naming a principal that was deleted or renamed, which otherwise "
                                   "runs and is refused everything it calls. If the application was running as a technical principal EAP "
                                   "issued it, that principal and its grants are deleted with the change: it was made for this "
                                   "application and nothing else can use it.",
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

        boost::json::object request{{"applicationId", vm["application-id"].as<std::string>()}};
        if (vm.contains("runtime")) request["runtime"] = vm["runtime"].as<std::string>();
        if (vm.contains("artifact")) request["artifact"] = vm["artifact"].as<std::string>();
        if (vm.contains("command")) request["command"] = vm["command"].as<std::string>();
        if (vm.contains("user")) request["user"] = vm["user"].as<std::string>();
        if (vm.contains("arguments")) request["arguments"] = SplitList(vm["arguments"].as<std::string>());
        if (vm.contains("environment")) request["environment"] = SplitEnvironment(vm["environment"].as<std::string>());
        if (vm.contains("buckets")) request["buckets"] = SplitList(vm["buckets"].as<std::string>());
        if (vm.contains("queues")) request["queues"] = SplitList(vm["queues"].as<std::string>());
        if (vm.contains("node-labels")) request["nodeLabels"] = SplitLabels(vm["node-labels"].as<std::string>());
        if (vm.contains("manifest") && !addManifestResources(vm["manifest"].as<std::string>(), request)) return 1;
        if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
        if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        if (vm.contains("type")) request["type"] = vm["type"].as<std::string>();
        if (vm.contains("schedule")) request["schedule"] = vm["schedule"].as<std::string>();
        if (vm.contains("ready-timeout")) request["readyTimeoutMs"] = vm["ready-timeout"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "update-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: update-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::copyApplication(const std::vector<std::string> &args) const {
        po::options_description desc("copy an application into another namespace");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to copy")
                ("to-namespace,t", po::value<std::string>()->required(), "namespace to copy it into")
                ("as", po::value<std::string>(), "name the copy is defined under; the original's name unless given");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "copy-application",
                                   "--application-id <name> --to-namespace <namespace> [--as <name>]",
                                   "Defines the same application again in another namespace, leaving the original alone and running - "
                                   "which is how a build is promoted from development to integration to production without taking the "
                                   "namespace it came from out of service. Use \"eap update-application --namespace\" instead to move an "
                                   "application rather than copy it. "
                                   "The copy runs the same artifact: bucket, object key and checksum are taken as they stand, so it is "
                                   "the same bytes and not a rebuild that happens to share a version. "
                                   "It is given its own runtime name and its own technical principal with its own access key, because "
                                   "both are installation-wide and cannot be shared - so revoking the copy's credentials leaves the "
                                   "original running. An application told to run as a named user keeps that user. "
                                   "What it may reach is re-resolved rather than copied: a bucket or queue ERN carries the namespace it "
                                   "was resolved in, so copying the list would point the new application at the old namespace's data. "
                                   "The same names are looked up in the target namespace instead, and a name that does not exist there "
                                   "fails the copy rather than quietly leaving the application with less access than the original. "
                                   "The copy is created stopped, whatever the original is doing - start it with \"eap start-application\" "
                                   "once you have looked at it.",
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

        boost::json::object request{
                {"applicationId", vm["application-id"].as<std::string>()},
                {"targetNamespace", vm["to-namespace"].as<std::string>()},
        };
        if (vm.contains("as")) request["targetApplicationId"] = vm["as"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "copy-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: copy-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::scaleApplication(const std::vector<std::string> &args) const {
        po::options_description desc("change how many instances an application runs");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to scale")
                ("min-instances", po::value<long>(), "smallest number of instances to keep running; left alone if not given")
                ("max-instances", po::value<long>(), "largest number the autoscaler may run; left alone if not given")
                ("instances,i", po::value<long>(), "run exactly this many: sets both bounds, which pins the pool and leaves the autoscaler no room");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "scale-application",
                                   "--application-id <name> [--instances <n>] [--min-instances <n>] [--max-instances <n>]",
                                   "Changes how many instances an application runs, without restarting the ones it already has. "
                                   "\"eap update-application\" can set the same two fields, but it writes the whole definition and "
                                   "stamps the modification date - and the manager restarts a pool whose application changed since it "
                                   "started it. Scaling that way therefore stops every running instance and starts it again, which is "
                                   "the opposite of what asking for more capacity means and worst at the moment somebody asks for it. "
                                   "What is set is the range the autoscaler works within, not a count: the manager scales toward it "
                                   "within a few seconds, adding instances one at a time and stopping idle ones as the load allows. "
                                   "--instances is shorthand for setting both bounds to the same number, which pins the pool at that "
                                   "size and leaves the autoscaler nothing to decide - useful to hold an application steady, and worth "
                                   "undoing afterwards. A bound not named is left as it stands, so a ceiling can be raised without "
                                   "touching the floor; the two are checked against each other, so a floor cannot be left above a "
                                   "ceiling. A floor of zero is refused: an application desired RUNNING with no instances reads "
                                   "everywhere as a pool that has failed - use \"eap stop-application\" to take one out of service.",
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

        if (vm.contains("instances") && (vm.contains("min-instances") || vm.contains("max-instances"))) {
            std::cerr << "error: --instances sets both bounds, so it cannot be combined with --min-instances or --max-instances" << std::endl;
            return 1;
        }
        if (!vm.contains("instances") && !vm.contains("min-instances") && !vm.contains("max-instances")) {
            std::cerr << "error: nothing to change - give --instances, --min-instances or --max-instances" << std::endl;
            return 1;
        }

        boost::json::object request{{"applicationId", vm["application-id"].as<std::string>()}};
        if (vm.contains("instances")) {
            const auto instances = vm["instances"].as<long>();
            request["minInstances"] = instances;
            request["maxInstances"] = instances;
        } else {
            if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
            if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "scale-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: scale-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
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

        // "ern:eqs:eu-central-1:000000000000:development:queue:orders" -> "queue orders", which is
        // how a person names it. The ERN itself when it is not one of those.
        std::string Describe(const std::string &ern) {
            std::vector<std::string> parts;
            std::size_t from = 0;
            for (int i = 0; i < 5; ++i) {
                const auto colon = ern.find(':', from);
                if (colon == std::string::npos) return ern;
                from = colon + 1;
            }
            const auto colon = ern.find(':', from);
            if (colon == std::string::npos) return ern;
            return ern.substr(from, colon - from) + " " + ern.substr(colon + 1);
        }

        // "access-queue-consume on <ern>" -> "access-queue-consume on queue orders".
        std::string DescribeGrant(const std::string &grant) {
            const auto on = grant.find(" on ");
            return on == std::string::npos ? grant : grant.substr(0, on) + " on " + Describe(grant.substr(on + 4));
        }

        // The plan EAP answered with, one line per step. Answers how many steps there were.
        std::size_t PrintPlan(const boost::json::value &plan) {

            std::size_t steps = 0;
            const auto each = [&](const char *field, const auto &print) {
                const auto *entries = plan.is_object() ? plan.as_object().if_contains(field) : nullptr;
                if (entries == nullptr || !entries->is_array()) return;
                for (const auto &entry: entries->as_array()) {
                    print(entry);
                    ++steps;
                }
            };
            const auto text = [](const boost::json::value &value, const char *field = nullptr) {
                const auto &v = field == nullptr ? value : value.as_object().at(field);
                return v.is_string() ? std::string(v.as_string()) : boost::json::serialize(v);
            };

            each("created", [&](const auto &e) { std::cout << "  create  " << Describe(text(e)) << "\n"; });
            each("adopted", [&](const auto &e) { std::cout << "  adopt   " << Describe(text(e)) << " - existed untagged, now recorded as this application's\n"; });
            each("updated", [&](const auto &e) {
                std::cout << "  update  " << Describe(text(e, "ern")) << ":";
                bool first = true;
                for (const auto &change: e.as_object().at("changes").as_array()) {
                    std::cout << (first ? " " : ", ") << change.as_string();
                    first = false;
                }
                std::cout << "\n";
            });
            each("deleted", [&](const auto &e) { std::cout << "  remove  " << Describe(text(e)) << " - no longer declared\n"; });
            each("kept", [&](const auto &e) { std::cout << "  keep    " << Describe(text(e, "ern")) << " - " << text(e, "reason") << "\n"; });
            each("granted", [&](const auto &e) { std::cout << "  grant   " << DescribeGrant(text(e)) << "\n"; });
            each("revoked", [&](const auto &e) { std::cout << "  revoke  " << DescribeGrant(text(e)) << "\n"; });
            return steps;
        }

    }// namespace

    // `apply` and `deploy-infrastructure` are one command. They used to be two, with two readers of
    // the folder and two engines - one in this CLI and one in EAP - which disagreed about settings,
    // about whose an object was, and about access. Now the folder is read here, and EAP does the
    // rest, exactly as it does on every deploy.
    int EapCli::deployInfrastructure(const std::vector<std::string> &args) const {

        po::options_description desc("apply options");
        desc.add_options()
                ("application-id,n", po::value<std::string>(), "the application the folder belongs to")
                ("platform,P", po::value<std::string>()->implicit_value("platform"),
                 "apply the folder as a platform - the shared objects no application owns - under this name; \"platform\" if none")
                ("directory,d", po::value<std::string>(), "the application's euclid/ folder; defaults to ./euclid")
                ("dir", po::value<std::string>(), "the same as --directory")
                ("dry-run", po::bool_switch(), "say what would be done, and do none of it")
                ("no-prune", po::bool_switch(), "keep owned objects the folder no longer declares, even when they are empty")
                ("json", po::bool_switch(), "print EAP's answer as JSON rather than one line per step");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "apply",
                                   "(--application-id <name> | --platform [<name>]) [--directory <folder>] [--dry-run] [--no-prune] [--json]",
                                   "Makes the installation match an application's euclid/ folder. The folder is merged into one "
                                   "declaration, stored beside the artifact as \"<application-id>.euclid.json\" - which is what every "
                                   "later create, update and redeploy applies again - and applied by EAP. With --dry-run nothing is "
                                   "stored or changed: EAP is handed the folder and answers what it would do.\n\n"
                                   "What the application creates is made with the settings its file names - a queue's visibility, "
                                   "maxRetries, maxMessageLength, delay, retention, priority and deadLetterQueue; a topic's "
                                   "maxMessageLength and retention - and an existing one has its named settings brought into line. "
                                   "A setting left out is left alone. An object that exists untagged is adopted; one another "
                                   "application owns is refused.\n\n"
                                   "What it owned and no longer declares is removed, but only when empty: a queue with messages, a "
                                   "bucket with objects or a topic with subscriptions is kept and listed. --no-prune keeps them all.\n\n"
                                   "What it uses is resolved, never created, and its access grants are made exactly the ones the "
                                   "folder asks for.\n\n"
                                   "--platform applies a folder of the shared objects no application owns - the bucket an FTP server "
                                   "writes into, a topic several services publish to. It may only create; what it owns is tagged "
                                   "\"platform:<name>\", and applications reach it with uses. See docs/application-manifest.md.",
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

        if (vm.contains("application-id") == vm.contains("platform")) {
            std::cerr << "error: name either --application-id or --platform, not both and not neither" << std::endl;
            return 1;
        }
        const auto platform = vm.contains("platform") ? vm["platform"].as<std::string>() : std::string{};
        const auto applicationId = platform.empty() ? vm["application-id"].as<std::string>() : std::string{};
        const auto directory = vm.contains("directory") ? vm["directory"].as<std::string>()
                               : vm.contains("dir")     ? vm["dir"].as<std::string>()
                                                        : std::string("euclid");
        const auto dryRun = vm["dry-run"].as<bool>();
        const auto prune = !vm["no-prune"].as<bool>();
        const auto json = vm["json"].as<bool>();

        namespace Infra = Database::Entity::EAP::Infrastructure;
        auto loaded = Infra::LoadFolder(directory);
        if (!loaded.present) {
            std::cerr << "error: not a directory: " << directory << std::endl;
            return 1;
        }
        if (!loaded.error.empty()) {
            std::cerr << "error: " << directory << ": " << loaded.error << std::endl;
            return 1;
        }
        if (loaded.files == 0) {
            std::cerr << "error: no .json files in " << directory << std::endl;
            return 1;
        }

        // Stamped into the document, so that from here on the file says which application it is for
        // rather than relying on the key it happens to be stored under. EAP refuses one that names a
        // different application - which is what stops a mistyped --application-id handing one
        // service's queues and topics to another, for that one to delete on its next reconcile.
        // A platform's folder names no application; one that does is that application's.
        if (!platform.empty() && !loaded.declaration.applicationId.empty()) {
            std::cerr << "error: these files declare application \"" << loaded.declaration.applicationId
                    << "\"; a platform's folder names none" << std::endl;
            return 1;
        }
        if (platform.empty() && loaded.declaration.applicationId.empty()) loaded.declaration.applicationId = applicationId;
        if (platform.empty() && loaded.declaration.applicationId != applicationId) {
            std::cerr << "error: these files declare \"" << loaded.declaration.applicationId
                    << "\", not \"" << applicationId << "\"" << std::endl;
            return 1;
        }
        const auto declaration = Infra::Write(loaded.declaration);

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            boost::json::object request{{"applicationId", applicationId}, {"prune", prune}};

            if (!platform.empty()) {
                // No artifact to store it beside: the declaration travels with the request, and EAP
                // keeps it on the platform's record.
                request = {{"name", platform}, {"prune", prune}, {"declaration", declaration}};
            } else if (dryRun) {
                // Handed over rather than stored: a dry run must not replace the declaration the next
                // deploy applies.
                request["declaration"] = declaration;
            } else {
                // The application's own bucket rather than a configured one: the declaration belongs
                // beside the artifact, and where that is, is something only the application record
                // knows.
                const HttpResponse current = client.Post("eap", "get-application", boost::json::object{{"applicationId", applicationId}});
                if (!current.IsSuccess()) {
                    std::cerr << "error: get-application failed (HTTP " << current.statusCode << "): "
                            << boost::json::serialize(current.body) << std::endl;
                    return 1;
                }
                const auto bucketErn = Core::GetStringValue(current.body, "bucketErn");
                if (bucketErn.empty()) {
                    std::cerr << "error: " << applicationId << " has no bucket to store a declaration beside" << std::endl;
                    return 1;
                }

                // Written out and uploaded rather than sent inline: this is an ordinary object, and
                // uploadOneFile is the same put-object path every other upload takes.
                const auto key = Infra::ObjectKey(applicationId);
                const auto temporary = std::filesystem::temp_directory_path() / key;
                {
                    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                    out << boost::json::serialize(declaration);
                }
                const EsmCli esm(_endpoint, _authentication, _pretty, _caCertPath);
                boost::json::value uploaded;
                const auto result = esm.uploadOneFile(bucketErn, key, temporary.string(), 0, 0, uploaded);
                std::error_code ec;
                std::filesystem::remove(temporary, ec);
                if (result != 0) return result;
            }

            // A dry run is an action of its own: an EAP that predates it refuses it as unknown,
            // where a flag on apply-infrastructure would be ignored and the apply done for real.
            const auto action = !platform.empty() ? (dryRun ? "plan-platform" : "apply-platform")
                                                  : (dryRun ? "plan-infrastructure" : "apply-infrastructure");
            const HttpResponse applied = client.Post("eap", action, request);
            if (!applied.IsSuccess()) {
                std::cerr << "error: " << (dryRun ? "the folder cannot be applied" : "apply failed")
                        << " (HTTP " << applied.statusCode << "): " << Core::GetStringValue(applied.body, "error", boost::json::serialize(applied.body))
                        << std::endl;
                return 1;
            }
            if (json) {
                Core::WriteJson(std::cout, applied.body, _pretty);
                return 0;
            }

            const auto nameSpace = _authentication.nameSpace.empty() ? std::string("the session's namespace") : "namespace " + _authentication.nameSpace;
            std::cout << (platform.empty() ? "Application '" + applicationId + "'" : "Platform '" + platform + "'")
                      << " in " << nameSpace << (dryRun ? ", dry run" : "") << ":\n";
            const auto steps = PrintPlan(applied.body);
            if (steps == 0) std::cout << "  nothing to change\n";
            if (dryRun) std::cout << "\n--dry-run: nothing was changed.\n";
            return 0;

        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::redeployApplication(const std::vector<std::string> &args) const {
        po::options_description desc("deploy a new build of an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to deploy")
                ("file,f", po::value<std::string>()->required(), "the new build: the jar for JAVA, the script for PYTHON/NODEJS, the executable for BINARY")
                ("artifact,a", po::value<std::string>(), "object key to store it under; defaults to the key the application already uses")
                ("version", po::value<std::string>(), "version this build is; read out of the file name (x.y.z) when not given")
                ("force", po::bool_switch(), "deploy even when the build is byte for byte the one already deployed");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "redeploy-application", "--application-id <name> --file <path> [--artifact <key>] [--version <x.y.z>] [--force]",
                                   "Deploys a new build: uploads the local file into the application's own bucket, under the artifact "
                                   "key the application already uses, and stamps the definition so the manager restarts the running "
                                   "instances onto it - within a few seconds, since an artifact is decided when a process starts. "
                                   "The version is read out of the file name (\"orders-1.4.0.jar\" is 1.4.0) unless --version says "
                                   "otherwise; the same version can be deployed again, since a rebuilt snapshot keeps its number. "
                                   "What is refused is a build that is byte for byte the one already deployed: there would be nothing "
                                   "to deploy, and the restart would buy nothing. Checked here before the upload, so a refused redeploy "
                                   "leaves the bucket as it was, and again by the server. "
                                   "--force deploys it anyway, for when the same bytes are the point: an artifact that was deleted and "
                                   "is being put back, or a host that lost its copy of the build. The upload happens and the definition "
                                   "is stamped, so the manager restarts the pool onto it - which is the part that was wanted. "
                                   "Give --artifact to deploy under a different key, for a versioned name (\"orders-1.4.0.jar\"); the "
                                   "application is repointed at it. An application that is stopped stays stopped and comes up on the "
                                   "new build when it is started.",
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

        const auto applicationId = vm["application-id"].as<std::string>();
        const auto filePath = vm["file"].as<std::string>();

        // Checked before anything is asked of the server: a mistyped path is the likeliest thing
        // to be wrong here, and finding out after the definition was stamped would mean a restart
        // onto the build that is still there.
        if (std::error_code ec; !std::filesystem::is_regular_file(filePath, ec)) {
            std::cerr << "error: redeploy-application failed: '" << filePath << "' is not a file\n";
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            // Where the build has to go is the application's own business, not something the
            // caller should have to look up and pass in correctly.
            const HttpResponse current = client.Post("eap", "get-application", boost::json::object{{"applicationId", applicationId}});
            if (!current.IsSuccess()) {
                std::cerr << "error: redeploy-application failed (HTTP " << current.statusCode << "): " << boost::json::serialize(current.body) << std::endl;
                return 1;
            }

            const auto &definition = current.body.as_object();
            const auto bucketErn = std::string(definition.at("bucketErn").as_string());
            const auto artifactKey = vm.count("artifact")
                                         ? vm["artifact"].as<std::string>()
                                         : std::string(definition.at("artifactKey").as_string());

            // What this build calls itself: what was asked for, else what the file's own name
            // says, else what the key says - "orders-1.4.0.jar" carries its version either way.
            auto version = vm.count("version")
                               ? vm["version"].as<std::string>()
                               : Database::Entity::EAP::VersionFromArtifactName(std::filesystem::path(filePath).filename().string());
            if (version.empty()) version = Database::Entity::EAP::VersionFromArtifactName(artifactKey);
            if (version.empty()) {
                std::cerr << "error: redeploy-application failed: no version in '" << std::filesystem::path(filePath).filename().string()
                        << "' (expected something like 1.4.0) - pass --version\n";
                return 1;
            }

            // The server refuses a redeploy that is not a new build, and would refuse this one
            // after the upload had already overwritten the artifact - leaving a build in the
            // bucket that was rejected but that the next restart would pick up anyway. So the same
            // two questions are asked here first, against the definition just read.
            const auto deployedVersion = definition.contains("version") ? std::string(definition.at("version").as_string()) : std::string();
            const auto deployedMd5 = definition.contains("md5Sum") ? std::string(definition.at("md5Sum").as_string()) : std::string();

            // Present, and not merely assumed to be: the checksum on this side was just taken over
            // the file about to be uploaded, so the bytes are in front of us. The server asks its
            // storage the same question, where the answer can be no.
            //
            // Not asked at all under --force: the answer is known and has been overridden, and
            // hashing the file only to ignore what it says costs a read of something that is
            // routinely hundreds of megabytes.
            const auto force = vm["force"].as<bool>();
            if (const auto refused = force
                                         ? std::string()
                                         : Database::Entity::EAP::RedeployRefusal(deployedVersion, deployedMd5, version,
                                                                                  Core::CryptoUtils::md5SumFile(filePath), true);
                !refused.empty()) {
                std::cerr << "error: refusing to redeploy '" << applicationId << "': " << refused
                        << "\npass --force to deploy it anyway, or use 'eap update-application --application-id " << applicationId
                        << " --artifact " << artifactKey << "'\n";
                return 1;
            }

            const EsmCli esm(_endpoint, _authentication, _pretty, _caCertPath);
            if (boost::json::value uploaded; esm.uploadOneFile(bucketErn, artifactKey, filePath, 0, 0, uploaded) != 0) {
                // uploadOneFile already said what went wrong, and the definition is untouched -
                // the application keeps running on the build it has.
                return 1;
            }

            // Recording the new version is what makes this a deployment rather than an upload: the
            // definition is stamped, and the manager reads that as a new revision and restarts the
            // pool onto the new artifact.
            //
            // The flag travels with it: the server asks the same question against what is in its
            // own storage, and the upload that just happened is exactly what it would refuse.
            const HttpResponse response = client.Post("eap", "redeploy-application",
                                                      boost::json::object{{"applicationId", applicationId}, {"artifact", artifactKey},
                                                                          {"version", version}, {"force", force}});
            if (!response.IsSuccess()) {
                std::cerr << "error: redeploy-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body)
                        << "\nthe new build was uploaded to " << artifactKey << "; run 'eap redeploy-application --application-id " << applicationId
                        << " --file " << filePath << "' again once that is resolved" << std::endl;
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;

        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::listApplications(const std::vector<std::string> &args) const {
        po::options_description desc("list applications");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "only list applications whose ID starts with this");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "list-applications", "[--prefix <prefix>]",
                                   "Lists the defined applications: their runtime, artifact, autoscaler bounds, the state they should "
                                   "be in, and how many instances the manager currently has running.",
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
        if (vm.contains("prefix")) request["prefix"] = vm["prefix"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "list-applications", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: list-applications failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::getApplication(const std::vector<std::string> &args) const {
        po::options_description desc("show an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "get-application", "--application-id <name>",
                                   "Shows one application's definition and how many instances of it are running.",
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
            const HttpResponse response = client.Post("eap", "get-application", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: get-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::deleteApplication(const std::vector<std::string> &args) const {
        po::options_description desc("delete an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to delete");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "delete-application", "--application-id <name>",
                                   "Deletes an application definition. Any instances the manager is running are stopped on its next "
                                   "reconcile - an application that no longer exists is not something it keeps running. The artifact "
                                   "in the bucket is left alone, but the technical principal created for the application - if it "
                                   "has one - is deleted with it, so no credential outlives what it was issued to.",
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
            const HttpResponse response = client.Post("eap", "delete-application", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::listNodes(const std::vector<std::string> &args) const {
        po::options_description desc("list worker nodes");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "list-nodes", "",
                                   "Lists the worker nodes registered in this account: the address, operating system and "
                                   "architecture each reported, its labels, cpu count and euclid version, whether it is drained, "
                                   "and whether it has renewed recently enough to be given work.",
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
            const HttpResponse response = client.Post("eap", "list-nodes", boost::json::object{});
            if (!response.IsSuccess()) {
                std::cerr << "error: list-nodes failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    // Sends one egress request and prints the answer - the three egress commands differ only in
    // what they send.
    int EapCli::postEgress(const std::string &action, const boost::json::object &request) const {
        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", action, request);
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

    int EapCli::listEgressSites(const std::vector<std::string> &args) const {
        po::options_description desc("list egress sites");
        desc.add_options()
                ("application-id,n", po::value<std::string>(), "only this application's sites");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "list-egress-sites", "[--application-id <id>]",
                                   "Lists, per application, every outside site it has asked an egress permit for or has a "
                                   "limit for: the limits it is held to (its own for the site, the application's \"*\" "
                                   "default, or euclid.modules.eap.egress.default), how many of its tokens are left, how "
                                   "many of its requests to the site are in flight, and whether the site has asked it to "
                                   "stay away - a 429 or 503 it reported - and until when.",
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
        if (vm.contains("application-id")) request["applicationId"] = vm["application-id"].as<std::string>();
        return postEgress("list-egress-sites", request);
    }

    int EapCli::setEgressLimit(const std::vector<std::string> &args) const {
        po::options_description desc("set an egress limit");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "the application the limit is for")
                ("site,s", po::value<std::string>()->required(), "host name of the outside site, e.g. downloads.example.com, or \"*\" for all of the application's sites without a limit of their own")
                ("requests-per-second,r", po::value<double>(), "requests per second all applications together may make, averaged; 0 for no rate limit")
                ("burst,b", po::value<double>(), "requests that may go at once before the rate applies (1 or more)")
                ("concurrent,c", po::value<long>(), "requests to the site in flight at once; 0 for no limit");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "set-egress-limit", "--application-id <id> --site <host|*> [--requests-per-second <n>] [--burst <n>] [--concurrent <n>]",
                                   "Sets how fast one application, all its instances on all nodes together, may call one "
                                   "outside site. Each application has its own limits: another calling the same site is "
                                   "not affected. --site \"*\" sets the application's default for every site that has no "
                                   "limit of its own; without either, euclid.modules.eap.egress.default applies. Any of the "
                                   "three values may be given on its own; the others keep what is in force. Applies to every "
                                   "EAP instance at once and survives a restart. Only applications that ask for a permit "
                                   "before each request are held to it - see the euclid-jdk EuclidEgressLimiter.",
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
        if (!vm.contains("requests-per-second") && !vm.contains("burst") && !vm.contains("concurrent")) {
            std::cerr << "error: at least one of --requests-per-second, --burst or --concurrent is required" << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request{{"applicationId", vm["application-id"].as<std::string>()}, {"site", vm["site"].as<std::string>()}};
        if (vm.contains("requests-per-second")) request["requestsPerSecond"] = vm["requests-per-second"].as<double>();
        if (vm.contains("burst")) request["burst"] = vm["burst"].as<double>();
        if (vm.contains("concurrent")) request["concurrent"] = vm["concurrent"].as<long>();
        return postEgress("set-egress-limit", request);
    }

    int EapCli::deleteEgressLimit(const std::vector<std::string> &args) const {
        po::options_description desc("delete an egress limit");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "the application the limit is for")
                ("site,s", po::value<std::string>()->required(), "host name of the outside site, or \"*\" for the application's default");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "delete-egress-limit", "--application-id <id> --site <host|*>",
                                   "Removes an application's limit for one outside site, so it is held to the application's "
                                   "\"*\" default again, or to euclid.modules.eap.egress.default if it has none. With --site "
                                   "\"*\", removes the application's default. What is in flight stays counted.",
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
        return postEgress("delete-egress-limit", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()},
                                                                     {"site", vm["site"].as<std::string>()}});
    }

    int EapCli::deleteEgressSite(const std::vector<std::string> &args) const {
        po::options_description desc("delete an egress site");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "the application the site belongs to")
                ("site,s", po::value<std::string>()->required(), "host name of the outside site, or \"*\" for the application's default");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "delete-egress-site", "--application-id <id> --site <host|*>",
                                   "Forgets one outside site of one application entirely: its limit, its token bucket, its "
                                   "concurrency slots and when it was last called. Where delete-egress-limit takes back a "
                                   "decision and leaves the site listed, this leaves nothing.\n\n"
                                   "Not a ban. A site is listed because the application asked for a permit to call it, so "
                                   "one it still calls is listed again on its next request, held to whatever it inherits. "
                                   "This is for the site that is finished with - a decommissioned host, or a limit set on a "
                                   "name that turned out to be a typo.\n\n"
                                   "Anything in flight for the site loses its slots at once: the permits were counted in "
                                   "the state this removes. The release that follows finds nothing and does nothing.",
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
        return postEgress("delete-egress-site", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()},
                                                                    {"site", vm["site"].as<std::string>()}});
    }

    int EapCli::getNode(const std::vector<std::string> &args) const {
        po::options_description desc("show a worker node");
        desc.add_options()
                ("node,n", po::value<std::string>()->required(), "name of the node");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "get-node", "--node <name>",
                                   "Shows one worker node's registration, as list-nodes shows each of them.",
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
            const HttpResponse response = client.Post("eap", "get-node", boost::json::object{{"node", vm["node"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: get-node failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::deleteNode(const std::vector<std::string> &args) const {
        po::options_description desc("delete a worker node");
        desc.add_options()
                ("node,n", po::value<std::string>()->required(), "name of the node to delete");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "delete-node", "--node <name>",
                                   "Removes a worker node's registration. A node name belongs to the principal that first "
                                   "registered it, and this is how it is freed for another. It does not stop the node: the "
                                   "instances assigned to it keep their leases, and a worker that is still running registers "
                                   "again on its next renewal. To take a node out of service, stop its euclid-wrk first; its "
                                   "leases then expire and its instances are placed elsewhere.",
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
            const HttpResponse response = client.Post("eap", "delete-node", boost::json::object{{"node", vm["node"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-node failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::setState(const std::vector<std::string> &args, const bool start) const {
        const std::string action = start ? "start-application" : "stop-application";

        po::options_description desc(start ? "start an application" : "stop an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", action, "--application-id <name>",
                                   start
                                       ? "Records that this application should be running. The manager's reconciler picks that up "
                                       "within a few seconds, materialises the artifact from its bucket and starts it; from then "
                                       "on the autoscaler runs between the application's minimum and maximum instance counts."
                                       : "Records that this application should be stopped. The manager's reconciler stops every "
                                       "instance on its next pass. The definition and the artifact are left in place, so "
                                       "\"start-application\" brings it back as it was.",
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
            const HttpResponse response = client.Post("eap", action, boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
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

    int EapCli::rebalanceApplication(const std::vector<std::string> &args) const {

        po::options_description desc("rebalance an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "rebalance-application", "--application-id <name>",
                                   "Asks the manager to spread this application's instances over the worker nodes again, the "
                                   "way placement would put them today - onto a node that has joined since they were placed, off "
                                   "one that holds more than its share for its cores, off one that is drained or no longer "
                                   "matches the application's node labels.\n\n"
                                   "The manager does this by itself after an instance goes away; this is for every other time. "
                                   "It moves one instance per pass: each move stops an instance and starts its replacement on "
                                   "the better node on the next pass, so the pool is at most one instance short while a move is "
                                   "under way, and an application that is already balanced is left exactly as it is. Nothing "
                                   "else restarts. Applies even when euclid.modules.eap.rebalance turns the automatic kind off.\n\n"
                                   "Returns as soon as the request is recorded; \"eap get-application\" and \"emm list-modules\" "
                                   "show where the instances are as they move. A JOB is refused: moving one of its runs would "
                                   "start that run again.",
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
            const HttpResponse response = client.Post("eap", "rebalance-application",
                                                      boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: rebalance-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::restartApplication(const std::vector<std::string> &args) const {

        po::options_description desc("restart an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "restart-application", "--application-id <name>",
                                   "Records that this application's instances should be started again. The manager's reconciler "
                                   "stops the whole pool on its next pass and starts it straight back up from the current "
                                   "definition - the same thing it does after a redeploy, with nothing new to pick up.\n\n"
                                   "The application keeps running as far as its desired state is concerned, so nothing is left "
                                   "stopped if this command, or the connection carrying it, does not survive the restart. An "
                                   "application that is stopped is refused rather than started: use \"eap start-application\".\n\n"
                                   "Reach for this when an instance has to re-do what it does at startup - a listener that has to "
                                   "subscribe again, a cache read once - rather than to deploy anything: the artifact, the "
                                   "environment and the credentials all come back as they were. Use \"eap redeploy-application\" "
                                   "for a new build.\n\n"
                                   "The whole pool goes down and comes back, which is what makes it a restart rather than "
                                   "\"emm restart-module\" - that cycles a module's instances one per reconcile tick, so it keeps "
                                   "serving throughout and no two instances are down together.",
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
            const HttpResponse response = client.Post("eap", "restart-application",
                                                      boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: restart-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::setLogLevel(const std::vector<std::string> &args) const {
        po::options_description desc("set an application's log level");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application")
                ("level,l", po::value<std::string>()->required(), "trace, debug, info, warning, error, fatal, off, or \"default\" to go back to the configured level");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "set-log-level", "--application-id <name> --level <level>",
                                   "Sets the level an application's own output is logged at, and takes effect on the manager's "
                                   "next reconcile - within a few seconds, without restarting the application or interrupting "
                                   "anything it is doing. "
                                   "Whatever an application writes to standard output or standard error is read back by the "
                                   "manager and logged on a channel of its own, \"app.<application-id>\"; this is that channel's "
                                   "level. Standard error is recorded as an error and standard output as information, so "
                                   "--level error leaves what went wrong visible while silencing the rest, and --level off "
                                   "silences the application entirely. "
                                   "--level default takes the setting back, leaving the application under euclid.logging.channels. "
                                   "The level is stored with the application, so it survives a restart of the installation.",
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

        // "default" is the word for taking the setting back; the server reads an empty level that
        // way. Asking somebody to pass an empty string for it would be a worse interface.
        auto level = vm["level"].as<std::string>();
        if (level == "default") level.clear();

        // Checked here as well as on the server, for the error message rather than for the rule:
        // a typo is worth answering now instead of after a round trip that ends in a 400.
        if (!level.empty() && !Core::LogStream::CanonicalLevel(level).has_value()) {
            std::cerr << "error: not a log level: " << level << std::endl
                    << "       expected trace, debug, info, warning, error, fatal, off, or default" << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "set-log-level",
                                                      boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}, {"level", level}});
            if (!response.IsSuccess()) {
                std::cerr << "error: set-log-level failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::platformInfo(const std::string &action, const std::vector<std::string> &args) const {

        po::options_description desc(action + " options");
        if (action == "get-platform") desc.add_options()("name,n", po::value<std::string>()->default_value("platform"), "the platform's name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", action, action == "get-platform" ? "[--name <name>]" : "",
                                   action == "get-platform"
                                           ? "Shows a platform declaration: the objects it owns and the declaration last applied with "
                                             "\"eap apply --platform\"."
                                           : "Lists the platform declarations of the namespace, with what each owns.",
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

        boost::json::object request;
        if (action == "get-platform") request["name"] = vm["name"].as<std::string>();
        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", action, request);
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

    int EapCli::applyManifest(const std::vector<std::string> &args) const {
        return deployInfrastructure(args);
    }

}// namespace Euclid::CLI